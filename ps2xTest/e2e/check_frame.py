#!/usr/bin/env python3
"""Check a frame dumped by PS2X_DUMP_FRAME (binary PPM) against expected size and pixel colours.

Usage: check_frame.py <frame.ppm> <expectations.json>

Expectations: width, height, and either
  'rects': [[x0, y0, x1, y1, [r, g, b]], ...] painted in order (pixels x0 <= x < x1, y0 <= y < y1) to build the
           whole expected frame, compared pixel by pixel within 'tolerance' per channel (default 0), or
  'pixels': [[x, y, [r, g, b]], ...] spot checks.
"""
import hashlib
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
    pixels = pixels[:width * height * 3]
    print(f'frame check: sha256 {hashlib.sha256(pixels).hexdigest()}')
    errors = []
    checked = 0
    if (width, height) != (expectations['width'], expectations['height']):
        errors.append(f'frame is {width}x{height}, expected {expectations["width"]}x{expectations["height"]}')
    elif 'rects' in expectations:
        expected = bytearray(width * height * 3)
        for x0, y0, x1, y1, rgb in expectations['rects']:
            row = bytes(rgb) * (x1 - x0)
            for y in range(y0, y1):
                expected[(y * width + x0) * 3:(y * width + x1) * 3] = row
        tolerance = expectations.get('tolerance', 0)
        bad = [i // 3 for i in range(0, len(expected), 3)
               if max(abs(pixels[i + c] - expected[i + c]) for c in range(3)) > tolerance]
        checked = width * height
        if bad:
            xs, ys = [p % width for p in bad], [p // width for p in bad]
            first = bad[0]
            errors.append(f'{len(bad)} pixels differ (tolerance {tolerance}) within x {min(xs)}..{max(xs)},'
                          f' y {min(ys)}..{max(ys)}; first ({first % width},{first // width}) ='
                          f' {tuple(pixels[first * 3:first * 3 + 3])}, expected {tuple(expected[first * 3:first * 3 + 3])}')
    else:
        for x, y, rgb in expectations['pixels']:
            off = (y * width + x) * 3
            got = tuple(pixels[off:off + 3])
            checked += 1
            if got != tuple(rgb):
                errors.append(f'pixel ({x},{y}) = {got}, expected {tuple(rgb)}')
    for e in errors:
        print('frame check:', e)
    print(f'frame check: {width}x{height}, {checked} pixels compared, {len(errors)} mismatches')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
