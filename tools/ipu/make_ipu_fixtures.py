"""Builds the IPU MPEG fixtures (ps2xTest/src/ipu_mpeg_fixtures.h) with ffmpeg as an independent MPEG-1/MPEG-2 encoder
and reference decoder.

For each fixture ffmpeg encodes one synthetic intra picture (48x32, testsrc2 plus fixed noise; no third-party content)
and decodes it again to 4:2:0 YUV with its floating-point IDCT (-idct faani). This script parses only the headers it
needs (sequence header and extensions, picture coding extension, slice headers); the macroblock data is left to the
IPU. Each slice is stored with the bytes from its quantiser_scale_code through the next start code, so the IPU sees the
slice terminator.

The reference samples come from another decoder, not from a PS2: agreement with them checks the ISO/IEC 13818-2 /
11172-2 semantics only, never the PS2 IPU's own behaviour.

usage: make_ipu_fixtures.py <work dir> <out header>
"""
import subprocess
import sys
from pathlib import Path

WIDTH, HEIGHT = 48, 32
SOURCE = f'testsrc2=size={WIDTH}x{HEIGHT}:rate=25,noise=alls=24:allf=u'
# A loaded intra matrix (raster order, as ffmpeg's -intra_matrix takes it): coarser towards high frequencies.
CUSTOM_MATRIX = ','.join(str(8 + 3 * (u + v) + (u * v) % 5) for v in range(8) for u in range(8))
FIXTURES = [
    ('mpeg2_default', 'mpeg2video', ['-q:v', '6']),
    ('mpeg2_alternate_b15_nonlinear_dc9', 'mpeg2video',
     ['-q:v', '4', '-qmax', '28', '-intra_vlc', '1', '-alternate_scan', '1', '-non_linear_quant', '1', '-dc', '9',
      '-intra_matrix', CUSTOM_MATRIX]),
    ('mpeg2_fine_dc10', 'mpeg2video', ['-q:v', '1', '-qmin', '1', '-qmax', '1', '-dc', '10']),
    # Alternating lines favour field DCT (constant-bitrate control with luminance/darkness masking).
    ('mpeg2_field_dct', 'mpeg2video',
     ['-flags', '+ildct', '-b:v', '400k', '-minrate', '400k', '-maxrate', '400k', '-bufsize', '400k',
      '-lumi_mask', '0.5', '-dark_mask', '0.5', '-qmin', '2', '-qmax', '31'],
     f"testsrc2=size={WIDTH}x{HEIGHT}:rate=25,geq=lum='if(mod(Y\\,2)\\,200\\,40)+mod(X*7\\,23)"
     f"+if(gt(X\\,24)\\,Y*3\\,0)':cb='128+X':cr='128+Y'"),
    ('mpeg1', 'mpeg1video', ['-q:v', '2', '-qmin', '1']),
    # Rate-distortion QP choice per macroblock on a picture with flat, noisy and striped thirds.
    ('mpeg2_rd_quantiser', 'mpeg2video',
     ['-b:v', '300k', '-minrate', '300k', '-maxrate', '300k', '-bufsize', '300k', '-qmin', '2', '-qmax', '31',
      '-mbd', 'rd', '-mpv_flags', '+qp_rd', '-scplx_mask', '0.9', '-lumi_mask', '0.9'],
     f"testsrc2=size={WIDTH}x{HEIGHT}:rate=25,geq=lum='if(lt(X\\,16)\\,128\\,if(lt(X\\,32)\\,mod(X*Y*37\\,255)"
     f"\\,if(mod(Y\\,2)\\,220\\,20)))':cb='128':cr='128'"),
]
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
          60, 61, 54, 47, 55, 62, 63]
# ISO/IEC 13818-2 default intra matrix, raster order (w[v][u]).
DEFAULT_INTRA = [8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38,
                 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
                 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83]


class Bits:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos * 8

    def get(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.data[self.pos // 8] >> (7 - self.pos % 8)) & 1)
            self.pos += 1
        return v


def start_codes(data):
    i = 0
    while True:
        i = data.find(b'\x00\x00\x01', i)
        if i < 0 or i + 3 >= len(data):
            return
        yield i, data[i + 3]
        i += 3


def parse(data):
    # A video sequence ends with sequence_end_code (13818-2 6.2.1); ffmpeg's raw muxer omits it, so the last slice
    # would end without a start code. It is appended here when missing.
    if not data.endswith(b'\x00\x00\x01\xB7'):
        data += b'\x00\x00\x01\xB7'
    info = {'mpeg2': False, 'idp': 0, 'as': 0, 'ivf': 0, 'qst': 0, 'dtd': 0,
            'matrix': [DEFAULT_INTRA[ZIGZAG[k]] for k in range(64)], 'slices': []}
    codes = list(start_codes(data))
    for n, (pos, code) in enumerate(codes):
        b = Bits(data, pos + 4)
        if code == 0xB3:
            b.get(12 + 12 + 4 + 4 + 18 + 1 + 10 + 1)
            if b.get(1):
                info['matrix'] = [b.get(8) for _ in range(64)]  # transmitted in zigzag order
        elif code == 0xB5:
            ext = b.get(4)
            if ext == 1:
                info['mpeg2'] = True
            elif ext == 8:
                b.get(16)
                info['idp'] = b.get(2)
                structure = b.get(2)
                b.get(1)
                frame_pred_frame_dct = b.get(1)
                b.get(1)
                info['qst'], info['ivf'], info['as'] = b.get(1), b.get(1), b.get(1)
                info['dtd'] = int(structure == 3 and frame_pred_frame_dct == 0)
            elif ext == 3 and b.get(1):
                info['matrix'] = [b.get(8) for _ in range(64)]
        elif code == 0x00:
            b.get(10)
            if b.get(3) != 1:
                raise SystemExit('not an I picture')
        elif 0x01 <= code <= 0xAF:
            qsc = b.get(5)
            while b.get(1):  # extra_bit_slice / extra_information_slice
                b.get(8)
            end = codes[n + 1][0] + 4 if n + 1 < len(codes) else len(data)
            info['slices'].append({'row': code - 1, 'qsc': qsc, 'header_bits': b.pos - (pos + 4) * 8,
                                   'data': data[pos + 4:end]})
    return info


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def bytes_literal(name, data):
    rows = [', '.join(f'0x{x:02X}' for x in data[i:i + 20]) for i in range(0, len(data), 20)]
    return [f'    inline constexpr uint8_t {name}[] = {{'] + [f'        {r},' for r in rows] + ['    };']


def main():
    work, out = Path(sys.argv[1]), Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    lines = ['// Generated by tools/ipu/make_ipu_fixtures.py with ffmpeg (encoder and -idct faani reference decoder).',
             '// The reference samples are another decoder\'s output, not PS2 results. Do not edit.', '#pragma once', '',
             '#include <cstddef>', '#include <cstdint>', '', 'namespace ipu_mpeg_fixtures', '{',
             '    struct Slice { uint32_t row; uint32_t qsc; uint32_t headerBits; size_t size; const uint8_t *data; };',
             '    // control: CTRL bits IDP 16-17, AS 20, IVF 21, QST 22, MP1 23; dtd: macroblocks carry dct_type.',
             '    struct Fixture { const char *name; uint32_t control; bool dtd; const uint8_t *intraMatrix;',
             '                     size_t sliceCount; const Slice *slices; const uint8_t *yuv; };',
             f'    constexpr uint32_t kWidth = {WIDTH};', f'    constexpr uint32_t kHeight = {HEIGHT};']
    table = []
    for name, codec, args, *source in FIXTURES:
        stream, yuv = work / f'{name}.m2v', work / f'{name}.yuv'
        run(['ffmpeg', '-hide_banner', '-y', '-f', 'lavfi', '-i', source[0] if source else SOURCE, '-frames:v', '1',
             '-c:v', codec, '-g', '1', *args, '-f', codec, str(stream)])
        run(['ffmpeg', '-hide_banner', '-y', '-idct', 'faani', '-i', str(stream), '-f', 'rawvideo', '-pix_fmt',
             'yuv420p', str(yuv)])
        info = parse(stream.read_bytes())
        if not info['slices']:
            raise SystemExit(f'{name}: no slices')
        control = (info['idp'] << 16) | (info['as'] << 20) | (info['ivf'] << 21) | (info['qst'] << 22) | \
                  ((0 if info['mpeg2'] else 1) << 23)
        lines.append(f'    // {name}: {len(info["slices"])} slices, control 0x{control:08X}, dtd {info["dtd"]}')
        lines += bytes_literal(f'k_{name}_matrix', info['matrix'])
        lines += bytes_literal(f'k_{name}_yuv', yuv.read_bytes())
        slices = []
        for i, s in enumerate(info['slices']):
            lines += bytes_literal(f'k_{name}_slice{i}', s['data'])
            slices.append(f'{{{s["row"]}, {s["qsc"]}, {s["header_bits"]}, sizeof(k_{name}_slice{i}), k_{name}_slice{i}}}')
        lines.append(f'    inline constexpr Slice k_{name}_slices[] = {{{", ".join(slices)}}};')
        table.append(f'        {{"{name}", 0x{control:08X}u, {"true" if info["dtd"] else "false"}, k_{name}_matrix, '
                     f'{len(slices)}, k_{name}_slices, k_{name}_yuv}},')
    lines += ['    inline constexpr Fixture kFixtures[] = {'] + table + ['    };', '}', '']
    out.write_text('\n'.join(lines), encoding='utf-8')
    print('wrote', out)


main()
