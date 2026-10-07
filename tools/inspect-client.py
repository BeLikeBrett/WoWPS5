#!/usr/bin/env python3
"""Read a supplied Windows EXE's PE identity/imports. Never execute or copy it."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b'MZ':
            raise ValueError('Not a DOS/PE executable')
        self.pe = self.u32(0x3c)
        if data[self.pe:self.pe + 4] != b'PE\0\0':
            raise ValueError('Invalid PE signature')
        self.machine, self.sections = struct.unpack_from('<HH', data, self.pe + 4)
        self.optional_size = self.u16(self.pe + 20)
        self.optional = self.pe + 24
        self.magic = self.u16(self.optional)
        if self.magic not in (0x10b, 0x20b):
            raise ValueError('Unsupported optional-header format')
        minimum = 112 if self.magic == 0x20b else 96
        if self.optional_size < minimum or not 1 <= self.sections <= 96:
            raise ValueError('Invalid PE header size or section count')
        self.headers_size = self.u32(self.optional + 60)
        self.base = self.u64(self.optional + 24) if self.magic == 0x20b else self.u32(self.optional + 28)
        self.directories = self.optional + minimum
        self.directory_count = min(self.u32(self.directories - 4), (self.optional_size - minimum) // 8)
        table = self.optional + self.optional_size
        self.ranges = []
        for i in range(self.sections):
            section = table + i * 40
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from('<IIII', data, section + 8)
            self.ranges.append((rva, raw_size, raw_offset))

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def u64(self, offset):
        return struct.unpack_from('<Q', self.data, offset)[0]

    def offset(self, rva):
        if 0 <= rva < min(self.headers_size, len(self.data)):
            return rva
        for start, size, offset in self.ranges:
            if start <= rva < start + size and offset + rva - start < len(self.data):
                return offset + rva - start
        raise ValueError('RVA has no file-backed data')

    def dll(self, rva):
        start = self.offset(rva)
        end = self.data.find(b'\0', start, min(start + 4096, len(self.data)))
        if end < 0:
            raise ValueError('Unterminated DLL name')
        return self.data[start:end].decode('ascii').lower()

    def exported_u32(self, name):
        if not self.directory_count:
            return None
        rva, size = struct.unpack_from('<II', self.data, self.directories)
        if not rva:
            return None
        table = self.offset(rva)
        functions, names = self.u32(table + 20), self.u32(table + 24)
        if functions > 100000 or names > 100000:
            raise ValueError('Unreasonable export table size')
        function_table = self.offset(self.u32(table + 28))
        name_table = self.offset(self.u32(table + 32))
        ordinal_table = self.offset(self.u32(table + 36))
        for i in range(names):
            exported = self.dll(self.u32(name_table + i * 4))
            if exported == name.lower():
                ordinal = self.u16(ordinal_table + i * 2)
                if ordinal >= functions:
                    raise ValueError('Export ordinal out of range')
                address = self.u32(function_table + ordinal * 4)
                if rva <= address < rva + size:  # a forwarded symbol, not data
                    return None
                return self.u32(self.offset(address))
        return None

    def imports(self, delayed=False):
        index = 13 if delayed else 1
        if self.directory_count <= index:
            return []
        rva, size = struct.unpack_from('<II', self.data, self.directories + index * 8)
        if not rva or not size:
            return []
        length = 32 if delayed else 20
        start = self.offset(rva)
        result = []
        for i in range(min(size // length, 10000)):
            values = struct.unpack_from('<' + 'I' * (length // 4), self.data, start + i * length)
            if not any(values):
                return sorted(set(result))
            if delayed:
                name = values[1] if values[0] & 1 else values[1] - self.base
            else:
                name = values[3]
            result.append(self.dll(name))
        raise ValueError('Import table lacks a terminator')

def inspect(path):
    data = path.read_bytes()
    pe = PE(data)
    imports, delayed = pe.imports(), pe.imports(True)
    dlls = set(imports + delayed)
    return {
        'schema': 1, 'fileName': path.name, 'bytes': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'machine': {0x8664: 'x86_64', 0x14c: 'x86', 0xaa64: 'arm64'}.get(pe.machine, hex(pe.machine)),
        'peFormat': 'PE32+' if pe.magic == 0x20b else 'PE32',
        'imports': imports, 'delayImports': delayed,
        'd3d12SDKVersionExport': pe.exported_u32('D3D12SDKVersion'),
        'graphicsImportHints': sorted(dlls.intersection({'d3d12.dll', 'd3d11.dll', 'dxgi.dll', 'vulkan-1.dll', 'opengl32.dll'})),
        'limitations': 'Imports are hints; dynamic loading, active renderer, version and direct-launch login must be checked separately.',
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        report = inspect(args.executable)
    except (OSError, ValueError, struct.error, UnicodeDecodeError) as error:
        parser.exit(2, str(error) + '\n')
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end='')

if __name__ == '__main__':
    main()
