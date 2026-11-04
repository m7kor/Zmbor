# ppm2png.py <in.ppm> <out.png> : P6 -> PNG with the standard library only; prints basic frame statistics.
import sys, zlib, struct, hashlib
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
rows = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
colors = len(set(px[i:i+3] for i in range(0, len(px), 3*97)))
nonblack = sum(1 for i in range(0, len(px), 3) if px[i] | px[i+1] | px[i+2])
print(f'{w}x{h} sha256={hashlib.sha256(px).hexdigest()[:16]} sampled_colors={colors} nonblack_pixels={nonblack}/{w*h}')
