"""Read-only UE reflection/spawning anchor discovery; not a runtime engine adapter."""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs, CsError
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

from pe_image import PEFormatError, PEImage, verified_image


ANCHORS = {
    'byte_property': 'ByteProperty',
    'int_property': 'IntProperty',
    'object_property': 'ObjectProperty',
    'spawn_failure': 'SpawnActor failed.',
}
MAX_ANCHOR_MATCHES = 128
MAX_REFERENCES = 4096


@dataclass(frozen=True)
class Function:
    start: int
    end: int


def backed_range(image: PEImage, rva: int, size: int) -> int:
    if size <= 0:
        raise PEFormatError('range must be nonempty')
    try:
        start = image.rva_to_file_offset(rva)
        end = image.rva_to_file_offset(rva + size - 1)
    except ValueError as exc:
        raise PEFormatError(f'unbacked range: {exc}') from None
    # Endpoint checks alone could admit a range crossing a gap or section.
    section = next((s for s in image.sections
                    if s.virtual_address <= rva
                    and rva + size <= s.virtual_address + s.backed_extent), None)
    if section is None or end != start + size - 1:
        raise PEFormatError('range crosses section boundary')
    return start


def runtime_functions(data: bytes, image: PEImage) -> list[Function]:
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    optional = pe + 24
    directories = struct.unpack_from('<I', data, optional + 108)[0]
    if directories <= 3:
        raise PEFormatError('missing AMD64 exception directory')
    rva, size = struct.unpack_from('<II', data, optional + 112 + 3 * 8)
    if not rva or not size or size % 12:
        raise PEFormatError('invalid AMD64 exception directory')
    offset = backed_range(image, rva, size)
    functions = []
    previous_end = 0
    for pos in range(offset, offset + size, 12):
        start, end, unwind = struct.unpack_from('<III', data, pos)
        if not start or start >= end or start < previous_end:
            raise PEFormatError('invalid, overlapping or unsorted runtime function')
        backed_range(image, start, end - start)
        section = next(s for s in image.sections
                       if s.virtual_address <= start
                       < s.virtual_address + s.backed_extent)
        if not section.executable:
            raise PEFormatError('runtime function is not in executable section')
        if not unwind or unwind % 4:
            raise PEFormatError('invalid unwind-info address')
        backed_range(image, unwind, 4)
        functions.append(Function(start, end))
        previous_end = end
    return functions


def find_anchors(data: bytes, image: PEImage) -> list[dict]:
    matches = []
    for name, text in ANCHORS.items():
        for encoding in ('ascii', 'utf-16-le'):
            pattern = text.encode(encoding) + (b'\0\0' if encoding == 'utf-16-le' else b'\0')
            for section in image.sections:
                if section.executable or not section.backed_extent:
                    continue
                begin = section.raw_offset
                end = begin + section.backed_extent
                pos = data.find(pattern, begin, end)
                while pos != -1:
                    rva = section.virtual_address + pos - begin
                    if encoding != 'utf-16-le' or rva % 2 == 0:
                        matches.append({'name': name, 'encoding': encoding, 'rva': rva})
                        if len(matches) > MAX_ANCHOR_MATCHES:
                            raise PEFormatError('anchor match limit exceeded')
                    pos = data.find(pattern, pos + 1, end)
    if not matches:
        raise PEFormatError('no known engine anchors found')
    return sorted(matches, key=lambda match: (match['rva'], match['name']))


def discover(data: bytes, image: PEImage) -> dict:
    anchors = find_anchors(data, image)
    functions = runtime_functions(data, image)
    targets = {anchor['rva'] for anchor in anchors}
    references = []
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    scanned_bytes = 0
    incomplete_functions = 0
    for function in functions:
        offset = backed_range(image, function.start, function.end - function.start)
        code = data[offset:offset + function.end - function.start]
        decoded_end = function.start
        # No skipdata: an invalid instruction stops this function rather than
        # turning arbitrary bytes into apparent instruction-boundary evidence.
        for instruction in decoder.disasm(code, function.start):
            decoded_end = instruction.address + instruction.size
            for operand in instruction.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                target = decoded_end + operand.mem.disp
                if target in targets:
                    references.append({
                        'instruction_rva': instruction.address,
                        'function_start_rva': function.start,
                        'function_end_rva': function.end,
                        'target_rva': target,
                        'mnemonic': instruction.mnemonic,
                    })
                    if len(references) > MAX_REFERENCES:
                        raise PEFormatError('reference limit exceeded')
        scanned_bytes += decoded_end - function.start
        incomplete_functions += decoded_end != function.end
    if not references:
        raise PEFormatError('no function-boundary-decoded references to engine anchors found')
    return {
        'status': 'offline_candidates_only',
        'runtime_engine_access': False,
        'anchors': anchors,
        'references': references,
        'coverage': {
            'runtime_functions': len(functions),
            'decoded_bytes': scanned_bytes,
            'incomplete_functions': incomplete_functions,
        },
        'limitations': [
            'String references are candidates, not validated engine functions or hook addresses.',
            'Exception-directory ranges omit leaf functions and may contain embedded data.',
            'No GNames, object layout, game-thread dispatcher or actor API is established.',
            'No gameplay changes, networking or multiplayer functionality.',
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--inventory', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        build, digest, data, image = verified_image(args.exe, args.manifest, args.inventory)
        result = discover(data, image)
        result.update({'build_id': build, 'sha256': digest, 'fingerprint_match': True})
    except (OSError, UnicodeError, PEFormatError, CsError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
