"""Bind an explicitly reviewed SDK leaf after verifying its ELF bytes by SHA-1.

This is analysis metadata, not a runtime address hook. It does not lower the
analyzer's conservative minimum signature size or guess short function names.
"""
import argparse
import hashlib
import pathlib
import re
import struct
try:
    import tomllib
except ModuleNotFoundError:  # Host pipeline also supports Python 3.10.
    tomllib = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=pathlib.Path)
    parser.add_argument('config', type=pathlib.Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--address', required=True, type=lambda s: int(s, 0))
    parser.add_argument('--size', required=True, type=lambda s: int(s, 0))
    parser.add_argument('--sha1', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.name):
        parser.error('invalid SDK identifier')
    if args.address < 0 or args.address % 4 or args.size <= 0 or args.size % 4:
        parser.error('address and positive size must be word aligned')
    data = args.elf.read_bytes()
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 8:
        parser.error('expected ELF32 little-endian MIPS')
    offset = struct.unpack_from('<I', data, 28)[0]
    entry_size, count = struct.unpack_from('<HH', data, 42)
    if entry_size < 32 or offset + entry_size * count > len(data):
        parser.error('invalid program header table')
    code = None
    for index in range(count):
        kind, file_offset, address, _, file_size, _, _, _ = struct.unpack_from('<8I', data, offset + index * entry_size)
        if kind == 1 and address <= args.address and args.address + args.size <= address + file_size:
            start = file_offset + args.address - address
            if start + args.size > len(data):
                parser.error('truncated load segment')
            code = data[start:start + args.size]
            break
    if code is None or hashlib.sha1(code).hexdigest() != args.sha1.lower():
        parser.error('SDK leaf signature mismatch; config left unchanged')
    text = args.config.read_text(encoding='utf-8-sig')
    if tomllib:
        config = tomllib.loads(text)
        stubs = config.get('general', {}).get('stubs', [])
    else:
        array = re.search(r'(?ms)^stubs\s*=\s*\[(.*?)\]', text)
        if not array:
            parser.error('expected an analyzer-generated stubs array')
        stubs = re.findall(r'"([^"\n]+)"', array[1])
    binding = f'{args.name}@0x{args.address:08X}'
    for stub in stubs:
        if '@' not in stub:
            continue
        name, address = stub.rsplit('@', 1)
        if int(address, 0) == args.address:
            if name != args.name:
                parser.error('address already has a different binding')
            print(f'already bound: {binding}')
            return
    updated, replacements = re.subn(r'(?m)^(stubs\s*=\s*\[)', lambda match: match[0] + f'\n  "{binding}",', text, count=1)
    if replacements != 1:
        parser.error('expected a stubs array')
    if tomllib:
        tomllib.loads(updated)  # Python 3.11+ validates the complete TOML too.
    args.config.write_text(updated, encoding='utf-8')
    print(f'bound verified SDK leaf: {binding} sha1={args.sha1.lower()}')


if __name__ == '__main__':
    main()
