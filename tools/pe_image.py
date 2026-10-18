#!/usr/bin/env python3
"""Offline, fingerprint-gated PE32+ section metadata and address translation."""
import argparse
import hashlib
import json
import re
import stat
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


class PEFormatError(ValueError):
    """Invalid, unsupported, or ambiguous PE image or fingerprint input."""


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000000)

    @property
    def extent(self):
        return max(self.virtual_size, self.raw_size)

    @property
    def backed_extent(self):
        return min(self.raw_size, self.virtual_size if self.virtual_size else self.raw_size)


@dataclass(frozen=True)
class PEImage:
    machine: int
    timestamp: int
    size_of_image: int
    size_of_headers: int
    image_base: int
    sections: tuple
    file_size: int

    def file_offset_to_rva(self, offset):
        _integer(offset)
        if offset < 0 or offset >= self.file_size:
            raise ValueError('file offset outside image')
        if offset < self.size_of_headers:
            return offset
        for section in self.sections:
            if section.backed_extent and section.raw_offset <= offset < section.raw_offset + section.backed_extent:
                return section.virtual_address + offset - section.raw_offset
        raise ValueError('file offset is overlay, padding, or unmapped data')

    def rva_to_file_offset(self, rva):
        _integer(rva)
        if rva < 0 or rva >= self.size_of_image:
            raise ValueError('RVA outside image')
        if rva < self.size_of_headers:
            return rva
        for section in self.sections:
            if section.backed_extent and section.virtual_address <= rva < section.virtual_address + section.backed_extent:
                return section.raw_offset + rva - section.virtual_address
        raise ValueError('RVA is zero-fill or unmapped data')


def _integer(value):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError('offset/RVA must be an integer')


def _need(data, offset, size, what):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise PEFormatError(f'truncated {what}')


def parse_image(data: bytes) -> PEImage:
    if not isinstance(data, bytes):
        raise PEFormatError('image must be bytes')
    _need(data, 0, 64, 'DOS header')
    if data[:2] != b'MZ':
        raise PEFormatError('missing MZ signature')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe < 64 or pe > len(data) - 24:
        raise PEFormatError('invalid or out-of-bounds e_lfanew')
    if data[pe:pe + 4] != b'PE\0\0':
        raise PEFormatError('missing PE signature')
    machine, count, timestamp, _, _, optional_size, _ = struct.unpack_from('<HHIIIHH', data, pe + 4)
    if machine != 0x8664:
        raise PEFormatError('only AMD64 images are supported')
    if not 1 <= count <= 96:
        raise PEFormatError('section count must be in 1..96')
    opt = pe + 24
    _need(data, opt, optional_size, 'optional header')
    if optional_size < 112:
        raise PEFormatError('optional header is too short')
    if struct.unpack_from('<H', data, opt)[0] != 0x20b:
        raise PEFormatError('only PE32+ images are supported')
    image_base = struct.unpack_from('<Q', data, opt + 24)[0]
    section_alignment, file_alignment = struct.unpack_from('<II', data, opt + 32)
    size_image, size_headers = struct.unpack_from('<II', data, opt + 56)
    directory_count = struct.unpack_from('<I', data, opt + 108)[0]
    if directory_count > (optional_size - 112) // 8:
        raise PEFormatError('data-directory table exceeds optional header')
    if not size_image or not size_headers or not section_alignment or not file_alignment:
        raise PEFormatError('invalid zero image/header/alignment size')
    table = opt + optional_size
    table_size = count * 40
    _need(data, table, table_size, 'section table')
    table_end = table + table_size
    if size_headers < table_end or size_headers > len(data) or size_headers > size_image:
        raise PEFormatError('SizeOfHeaders does not enclose section table within file')
    sections = []
    raw_ranges, virtual_ranges = [], []
    for i in range(count):
        pos = table + i * 40
        raw_name = data[pos:pos + 8].split(b'\0', 1)[0]
        name = raw_name.decode('ascii', errors='replace')
        virtual_size, va, raw_size, raw_offset = struct.unpack_from('<IIII', data, pos + 8)
        characteristics = struct.unpack_from('<I', data, pos + 36)[0]
        extent = max(virtual_size, raw_size)
        end = va + extent
        if end > 0xffffffff or end > size_image or (extent and va < size_headers):
            raise PEFormatError(f'invalid virtual extent in section {i}')
        if raw_size:
            raw_end = raw_offset + raw_size
            if raw_end > 0xffffffff or raw_offset < size_headers or raw_end > len(data):
                raise PEFormatError(f'invalid raw extent in section {i}')
            raw_ranges.append((raw_offset, raw_end))
        if extent:
            virtual_ranges.append((va, end))
        sections.append(Section(name, va, virtual_size, raw_offset, raw_size, characteristics))
    for ranges, label in ((raw_ranges, 'raw'), (virtual_ranges, 'virtual')):
        ranges.sort()
        if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
            raise PEFormatError(f'overlapping {label} section extents')
    return PEImage(machine, timestamp, size_image, size_headers, image_base, tuple(sections), len(data))


def _read_stable(path):
    def identity(st):
        return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns,
                getattr(st, 'st_ctime_ns', None))

    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise PEFormatError('input is not a regular file')
    try:
        with path.open('rb') as stream:
            opened = os.fstat(stream.fileno())
            if identity(opened) != identity(before):
                raise PEFormatError('file changed before read')
            data = stream.read(before.st_size + 1)
            descriptor_after = os.fstat(stream.fileno())
        after = path.stat()
    except OSError as exc:
        raise PEFormatError(f'cannot read stable file: {exc}') from None
    if (len(data) != before.st_size or identity(descriptor_after) != identity(before)
            or identity(after) != identity(before)):
        raise PEFormatError('file identity/metadata/size changed during read')
    return data


def _buildid(path):
    text = path.read_text(encoding='utf-8')
    matches = re.findall(r'"buildid"\s*"([^"\r\n]*)"', text)
    if len(matches) != 1 or not re.fullmatch(r'[0-9]+', matches[0]):
        raise PEFormatError('manifest must contain exactly one nonempty numeric buildid scalar')
    return matches[0]


def verified_image(exe, manifest, inventory):
    try:
        expected = json.loads(inventory.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PEFormatError(f'invalid inventory: {exc}') from None
    build = expected.get('game_build') if isinstance(expected, dict) else None
    digest = expected.get('game_sha256') if isinstance(expected, dict) else None
    if (not isinstance(build, str) or not re.fullmatch(r'[0-9]+', build)
            or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', digest)):
        raise PEFormatError('inventory requires numeric game_build and 64-hex string game_sha256')
    actual_build = _buildid(manifest)
    data = _read_stable(exe)
    actual_hash = hashlib.sha256(data).hexdigest()
    if actual_build != build or actual_hash.lower() != digest.lower():
        raise PEFormatError('build ID or executable SHA-256 does not match inventory')
    return actual_build, actual_hash, data, parse_image(data)


def report(exe, manifest, inventory):
    actual_build, actual_hash, _, image = verified_image(exe, manifest, inventory)
    return {'build_id': actual_build, 'sha256': actual_hash, 'fingerprint_match': True,
            'machine': image.machine, 'timestamp': image.timestamp, 'size_of_image': image.size_of_image,
            'size_of_headers': image.size_of_headers, 'preferred_image_base': image.image_base,
            'sections': [{'name': s.name, 'rva': s.virtual_address, 'virtual_size': s.virtual_size,
                         'raw_file_offset': s.raw_offset, 'raw_size': s.raw_size,
                         'characteristics': s.characteristics, 'executable_flag': s.executable} for s in image.sections],
            'limitation': 'offline only; no runtime/engine/co-op validation'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--inventory', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(report(args.exe, args.manifest, args.inventory), indent=2))
        return 0
    except (OSError, UnicodeError, PEFormatError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
