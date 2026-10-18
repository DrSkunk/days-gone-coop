import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import pe_image


def fixture():
    b = bytearray(0x800)
    b[:2] = b'MZ'
    struct.pack_into('<I', b, 0x3c, 0x80)
    b[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', b, 0x84, 0x8664, 2, 0x12345678, 0, 0, 0xf0, 0x22)
    o = 0x98
    struct.pack_into('<H', b, o, 0x20b)
    struct.pack_into('<Q', b, o + 24, 0x140000000)
    struct.pack_into('<II', b, o + 32, 0x1000, 0x200)
    struct.pack_into('<II', b, o + 56, 0x5000, 0x200)
    struct.pack_into('<I', b, o + 108, 16)
    t = o + 0xf0
    for i, (name, vs, va, rs, ro) in enumerate(((b'.text', 0x180, 0x1000, 0x200, 0x200), (b'.data', 0x100, 0x3000, 0x200, 0x500))):
        p = t + i * 40
        b[p:p + len(name)] = name
        struct.pack_into('<IIII', b, p + 8, vs, va, rs, ro)
        struct.pack_into('<I', b, p + 36, 0xc0000040 if i else 0x60000020)
    return bytes(b)


class PEImageTests(unittest.TestCase):
    def test_section_permissions(self):
        image = pe_image.parse_image(fixture())
        self.assertTrue(image.sections[0].executable)
        self.assertFalse(image.sections[1].executable)

    def test_ascii_build_ids_only(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / 'manifest'
            inventory = Path(td) / 'inventory'
            for value in ('١٢٣', '１２３', '', '123a', ' 123'):
                manifest.write_text(f'"buildid" "{value}"', encoding='utf-8')
                with self.assertRaises(pe_image.PEFormatError):
                    pe_image._buildid(manifest)
                inventory.write_text(json.dumps({'game_build': value, 'game_sha256': '0' * 64}))
                with self.assertRaisesRegex(pe_image.PEFormatError, 'inventory requires'):
                    pe_image.verified_image(Path(td) / 'missing', manifest, inventory)
            manifest.write_text('"buildid" "123"\n"buildid" "123"')
            with self.assertRaises(pe_image.PEFormatError):
                pe_image._buildid(manifest)

    def test_overlapping_and_out_of_bounds_sections(self):
        table = 0x98 + 0xf0
        for offset, value in ((table + 40 + 12, 0x1100),
                              (table + 40 + 20, 0x300),
                              (table + 40 + 16, 0xffffffff),
                              (table + 40 + 12, 0xffffffff)):
            data = bytearray(fixture())
            struct.pack_into('<I', data, offset, value)
            with self.assertRaises(pe_image.PEFormatError):
                pe_image.parse_image(bytes(data))

    def test_zero_virtual_size_and_invalid_translation_types(self):
        data = bytearray(fixture())
        struct.pack_into('<I', data, 0x98 + 0xf0 + 8, 0)
        image = pe_image.parse_image(bytes(data))
        self.assertEqual(image.file_offset_to_rva(0x3ff), 0x11ff)
        for value in (True, False, '12', 1.0, None):
            for translate in (image.file_offset_to_rva, image.rva_to_file_offset):
                with self.assertRaises(ValueError):
                    translate(value)

    def test_stable_read_detects_metadata_change(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'image'
            path.write_bytes(fixture())
            real_stat = path.stat()
            changed_stat = (Path(td)).stat()
            with patch.object(Path, 'stat', side_effect=[real_stat, changed_stat]):
                with self.assertRaisesRegex(pe_image.PEFormatError, 'changed during read'):
                    pe_image._read_stable(path)
            with self.assertRaises(pe_image.PEFormatError):
                pe_image._read_stable(Path(td))

    def test_translation_and_rejections(self):
        image = pe_image.parse_image(fixture())
        self.assertEqual(image.file_offset_to_rva(0x200), 0x1000)
        self.assertEqual(image.file_offset_to_rva(0x37f), 0x117f)
        self.assertEqual(image.rva_to_file_offset(0x3000), 0x500)
        for v in (0, 0x1ff, 0x200, 0x37f, 0x500, 0x5ff):
            self.assertEqual(image.rva_to_file_offset(image.file_offset_to_rva(v)), v)
        for call, value in ((image.file_offset_to_rva, 0x380), (image.file_offset_to_rva, 0x400),
                            (image.rva_to_file_offset, 0x1180), (image.rva_to_file_offset, 0x2000),
                            (image.file_offset_to_rva, -1), (image.rva_to_file_offset, 0x5000)):
            with self.assertRaises(ValueError):
                call(value)
        bss = bytearray(fixture())
        struct.pack_into('<IIII', bss, 0x98 + 0xf0 + 40 + 8, 0x300, 0x3000, 0, 0)
        bss[0x98 + 0xf0 + 40 + 8 + 8:0x98 + 0xf0 + 40 + 8 + 16] = struct.pack('<II', 0, 0)
        im = pe_image.parse_image(bytes(bss))
        with self.assertRaises(ValueError):
            im.rva_to_file_offset(0x3000)

    def test_structural_failures(self):
        good = fixture()
        for data in (good[:20], good[:0x90], good[:0x188], good[:0x200]):
            with self.assertRaises(ValueError): pe_image.parse_image(data)
        for offset, fmt, value in ((0x3c, '<I', 0xffffffff), (0x84, '<H', 0x14c), (0x98, '<H', 0x10b), (0x86, '<H', 97)):
            b = bytearray(good); struct.pack_into(fmt, b, offset, value)
            with self.assertRaises(ValueError): pe_image.parse_image(bytes(b))

    def test_size_headers_must_fit_image(self):
        b = bytearray(fixture())
        struct.pack_into('<II', b, 0x98 + 56, 0x100, 0x200)
        struct.pack_into('<HHHH', b, 0x86, 1, 0, 0, 0)
        struct.pack_into('<IIII', b, 0x98 + 0xf0 + 8, 0, 0, 0, 0)
        with self.assertRaises(ValueError):
            pe_image.parse_image(bytes(b))

    def test_stable_read_and_inventory_scalar_validation(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'data'
            p.write_bytes(b'fixture')
            self.assertEqual(pe_image._read_stable(p), b'fixture')
            for invalid in (1, True, [], {}, None, ''):
                inv = Path(td) / 'inventory.json'
                inv.write_text(json.dumps({'game_build': '123', 'game_sha256': invalid}))
                with self.assertRaises(pe_image.PEFormatError):
                    pe_image.report(p, Path(td) / 'missing-manifest', inv)

    def test_cli_fingerprint_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td); exe=d/'x.exe'; manifest=d/'appmanifest.acf'; inv=d/'inventory.json'
            data=fixture(); exe.write_bytes(data); manifest.write_text('"buildid" "123"')
            inv.write_text(json.dumps({'game_build':'123','game_sha256':hashlib.sha256(data).hexdigest()}))
            before=(exe.read_bytes(), manifest.read_bytes(), inv.read_bytes())
            cmd=[sys.executable, str(Path(__file__).with_name('pe_image.py')), '--exe',str(exe),'--manifest',str(manifest),'--inventory',str(inv)]
            p=subprocess.run(cmd,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr); self.assertTrue(json.loads(p.stdout)['fingerprint_match'])
            inv.write_text(json.dumps({'game_build':'wrong','game_sha256':hashlib.sha256(data).hexdigest()}))
            p=subprocess.run(cmd,capture_output=True,text=True)
            self.assertNotEqual(p.returncode,0); self.assertNotIn('"sections"',p.stdout)
            self.assertEqual((exe.read_bytes(),manifest.read_bytes()),before[:2])


if __name__ == '__main__':
    unittest.main()
