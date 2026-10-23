#!/usr/bin/env python3
"""Check a frame dumped by PS2X_DUMP_FRAME (binary PPM) against expected size and pixel colours.

Usage: check_frame.py <frame.ppm> <expectations.json>
"""
import json
import pathlib
import sys


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    if fields[0] != b'P6' or fields[3] != b'255':
        raise ValueError('not an 8-bit binary PPM')
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:]
    if len(pixels) < width * height * 3:
        raise ValueError('truncated PPM')
    return width, height, pixels


def main():
    frame, expectations = sys.argv[1], json.loads(pathlib.Path(sys.argv[2]).read_text(encoding='utf-8'))
    width, height, pixels = read_ppm(frame)
    errors = []
    if (width, height) != (expectations['width'], expectations['height']):
        errors.append(f'frame is {width}x{height}, expected {expectations["width"]}x{expectations["height"]}')
    else:
        for x, y, rgb in expectations['pixels']:
            off = (y * width + x) * 3
            got = tuple(pixels[off:off + 3])
            if got != tuple(rgb):
                errors.append(f'pixel ({x},{y}) = {got}, expected {tuple(rgb)}')
    for e in errors:
        print('frame check:', e)
    print(f'frame check: {width}x{height}, {len(expectations["pixels"])} pixels, {len(errors)} mismatches')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
