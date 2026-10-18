import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import engine_discovery as discovery
import pe_image
from test_pe_image import fixture


def engine_fixture(negative_displacement=False):
    data = bytearray(fixture())
    text_rva, data_rva = ((0x3000, 0x1000) if negative_displacement else (0x1000, 0x3000))
    table = 0x98 + 0xf0
    struct.pack_into('<I', data, table + 12, text_rva)
    struct.pack_into('<IIII', data, table + 40 + 8, 0x300, data_rva, 0x300, 0x500)
    ascii_anchor = b'ByteProperty\0'
    utf16_anchor = 'SpawnActor failed.\0'.encode('utf-16-le')
    data[0x500:0x500 + len(ascii_anchor)] = ascii_anchor
    data[0x540:0x540 + len(utf16_anchor)] = utf16_anchor
    for file_offset, start, target in ((0x200, text_rva, data_rva),
                                        (0x220, text_rva + 0x20, data_rva + 0x40)):
        data[file_offset:file_offset + 8] = b'\x48\x8d\x05' + struct.pack('<i', target - start - 7) + b'\xc3'
    struct.pack_into('<II', data, 0x98 + 112 + 3 * 8, data_rva + 0x200, 24)
    struct.pack_into('<III', data, 0x700, text_rva, text_rva + 8, data_rva + 0xc0)
    struct.pack_into('<III', data, 0x70c, text_rva + 0x20, text_rva + 0x28, data_rva + 0xc0)
    data[0x5c0:0x5c4] = b'\x01\0\0\0'
    return bytes(data)


class DiscoveryTests(unittest.TestCase):
    def inspect(self, data):
        return discovery.discover(data, pe_image.parse_image(data))

    def test_positive_and_negative_rip_displacements(self):
        for negative in (False, True):
            data = engine_fixture(negative)
            before = bytes(data)
            result = self.inspect(data)
            self.assertEqual(result['status'], 'offline_candidates_only')
            self.assertFalse(result['runtime_engine_access'])
            self.assertEqual(len(result['anchors']), 2)
            self.assertEqual(len(result['references']), 2)
            self.assertEqual(result['coverage'], {
                'runtime_functions': 2, 'decoded_bytes': 16, 'incomplete_functions': 0,
            })
            for anchor, reference in zip(result['anchors'], result['references']):
                self.assertEqual(anchor['rva'], reference['target_rva'])
                self.assertEqual(reference['mnemonic'], 'lea')
            self.assertEqual(data, before)

    def test_immediate_bytes_are_not_references(self):
        data = bytearray(engine_fixture())
        data[0x200:0x208] = b'\xb8\x00\x30\x00\x00\xc3\x90\x90'
        result = self.inspect(bytes(data))
        self.assertEqual(len(result['references']), 1)
        self.assertEqual(result['references'][0]['instruction_rva'], 0x1020)

    def test_does_not_scan_outside_runtime_function(self):
        data = bytearray(engine_fixture())
        data[0x210:0x217] = b'\x48\x8d\x05' + struct.pack('<i', 0x3000 - 0x1010 - 7)
        self.assertEqual(len(self.inspect(bytes(data))['references']), 2)

    def test_incomplete_disassembly_is_reported(self):
        data = bytearray(engine_fixture())
        struct.pack_into('<I', data, 0x98 + 112 + 3 * 8 + 4, 36)
        struct.pack_into('<III', data, 0x718, 0x1040, 0x1041, 0x30c0)
        data[0x240] = 0x0f
        result = self.inspect(bytes(data))
        self.assertEqual(result['coverage']['incomplete_functions'], 1)
        self.assertEqual(result['coverage']['decoded_bytes'], 16)

    def test_missing_anchors_and_references_fail_explicitly(self):
        with self.assertRaisesRegex(pe_image.PEFormatError, 'no known engine anchors'):
            self.inspect(fixture())
        data = bytearray(engine_fixture())
        data[0x200:0x208] = b'\x90' * 8
        data[0x220:0x228] = b'\x90' * 8
        with self.assertRaisesRegex(pe_image.PEFormatError, 'no function-boundary-decoded references'):
            self.inspect(bytes(data))

    def test_anchor_must_be_terminated_and_outside_executable_section(self):
        data = bytearray(engine_fixture())
        data[0x50c] = ord('x')
        result = self.inspect(bytes(data))
        self.assertEqual(len(result['anchors']), 1)
        self.assertEqual(result['anchors'][0]['name'], 'spawn_failure')
        struct.pack_into('<I', data, 0x98 + 0xf0 + 40 + 36, 0x60000020)
        with self.assertRaisesRegex(pe_image.PEFormatError, 'no known engine anchors'):
            self.inspect(bytes(data))

    def test_exception_directory_failures(self):
        directory = 0x98 + 112 + 3 * 8
        mutations = (
            (directory, 0), (directory + 4, 13),
            (directory, 0x4000), (0x700, 0x1008),
            (0x70c, 0x1004), (0x704, 0x2000),
            (0x708, 0), (0x708, 0x30c1), (0x708, 0x4000),
            (0x98 + 108, 3),
        )
        for offset, value in mutations:
            with self.subTest(offset=offset, value=value):
                data = bytearray(engine_fixture())
                struct.pack_into('<I', data, offset, value)
                with self.assertRaises(pe_image.PEFormatError):
                    self.inspect(bytes(data))

    def test_function_in_nonexecutable_section_is_rejected(self):
        data = bytearray(engine_fixture())
        struct.pack_into('<III', data, 0x700, 0x3000, 0x3008, 0x30c0)
        with self.assertRaisesRegex(pe_image.PEFormatError, 'not in executable'):
            self.inspect(bytes(data))

    def test_range_cannot_bridge_sections_or_use_virtual_fill(self):
        data = engine_fixture()
        image = pe_image.parse_image(data)
        for rva, size in ((0x1000, 0), (0x1170, 0x2000), (0x1180, 1), (0x31ff, 0x102)):
            with self.subTest(rva=rva, size=size):
                with self.assertRaises(pe_image.PEFormatError):
                    discovery.backed_range(image, rva, size)

    def test_result_limits_fail_explicitly(self):
        data = engine_fixture()
        with patch.object(discovery, 'MAX_ANCHOR_MATCHES', 1):
            with self.assertRaisesRegex(pe_image.PEFormatError, 'anchor match limit'):
                self.inspect(data)
        with patch.object(discovery, 'MAX_REFERENCES', 1):
            with self.assertRaisesRegex(pe_image.PEFormatError, 'reference limit'):
                self.inspect(data)

    def test_adjacent_sections_cannot_be_used_as_one_range(self):
        data = bytearray(engine_fixture())
        struct.pack_into('<IIII', data, 0x98 + 0xf0 + 40 + 8, 0x300, 0x1180, 0x300, 0x500)
        # Remove raw alignment padding from occupancy of the preceding section.
        struct.pack_into('<I', data, 0x98 + 0xf0 + 16, 0x180)
        image = pe_image.parse_image(bytes(data))
        with self.assertRaisesRegex(pe_image.PEFormatError, 'crosses section boundary'):
            discovery.backed_range(image, 0x117f, 2)

    def test_cli_is_fingerprint_gated_and_read_only(self):
        with tempfile.TemporaryDirectory() as td:
            directory = Path(td)
            exe, manifest, inventory = (directory / name for name in ('game.exe', 'manifest', 'inventory'))
            data = engine_fixture()
            exe.write_bytes(data)
            manifest.write_text('"buildid" "19221447"')
            inventory.write_text(json.dumps({
                'game_build': '19221447',
                'game_sha256': hashlib.sha256(data).hexdigest(),
            }))
            paths = (exe, manifest, inventory)
            original = [p.read_bytes() for p in paths]
            command = [sys.executable, str(Path(discovery.__file__)), '--exe', str(exe),
                       '--manifest', str(manifest), '--inventory', str(inventory)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertTrue(report['fingerprint_match'])
            self.assertEqual(len(report['references']), 2)
            self.assertEqual(original, [p.read_bytes() for p in paths])
            exe.write_bytes(data + b'changed')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')
            self.assertIn('does not match inventory', result.stderr)
            exe.unlink()
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, '')
            self.assertIn('error:', result.stderr)


if __name__ == '__main__':
    unittest.main()
