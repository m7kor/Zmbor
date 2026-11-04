# contact_sheet.py <series_dir> <out.png> [columns]: tiles every frame_<ms>.ppm (labelled with its time) and prints
# per-frame statistics (non-black pixels, distinct colours, content hash) so screen changes can be followed.
import hashlib
import pathlib
import sys

from PIL import Image, ImageDraw

src = pathlib.Path(sys.argv[1])
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
frames = sorted(src.glob('frame_*.ppm'))
thumbs = []
for f in frames:
    im = Image.open(f).convert('RGB')
    px = im.tobytes()
    nonblack = sum(1 for i in range(0, len(px), 3) if px[i] | px[i + 1] | px[i + 2])
    digest = hashlib.sha256(px).hexdigest()[:10]
    colors = len(im.getcolors(1 << 20) or [])
    ms = int(f.stem.split('_')[1])
    print(f'{ms / 1000:7.1f}s nonblack={nonblack:6d} colors={colors:5d} sha={digest}')
    t = im.resize((im.width // 2, im.height // 2))
    ImageDraw.Draw(t).text((4, 4), f'{ms / 1000:.0f}s', fill=(255, 255, 0))
    thumbs.append(t)
if thumbs:
    w, h = thumbs[0].size
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * w, rows * h))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * w, (i // cols) * h))
    sheet.save(sys.argv[2])
