"""Turn beauty.png + mask.png into a Jewel Case template: base.png, over.png, meta.json.
usage: jc_post.py <render dir> <out dir> <target area px> [rot90 count]
"""
import sys, os, json
import numpy as np
from PIL import Image

src, dst, area = sys.argv[1], sys.argv[2], float(sys.argv[3])
rot = int(sys.argv[4]) if len(sys.argv) > 4 else 0
os.makedirs(dst, exist_ok=True)
A = Image.open(os.path.join(src, 'beauty.png')).convert('RGBA')
Mi = Image.open(os.path.join(src, 'mask.png')).convert('RGBA')
if rot:
    A = A.rotate(-90 * rot, expand=True)   # clockwise
    Mi = Mi.rotate(-90 * rot, expand=True)
bb = A.getbbox()
A = A.crop(bb)
Mi = Mi.crop(bb)
sc = (area / (A.width * A.height)) ** 0.5
W, H = max(1, round(A.width * sc)), max(1, round(A.height * sc))
A = A.resize((W, H), Image.LANCZOS)
Mi = Mi.resize((W, H), Image.LANCZOS)

a = np.asarray(A, np.float32) / 255
mm = np.asarray(Mi, np.float32) / 255
m = np.clip(mm[..., 0], 0, 1) * (a[..., 3] > 0.01)
ys, xs = np.where(m > 0.5)
lx, ly, lx1, ly1 = int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1
# corners of the label rect a hair inside so no art bleeds past antialiased edges
L = (a[..., :3] * [0.2126, 0.7152, 0.0722]).sum(-1)
ref = float(np.median(L[m > 0.95]))
s = np.clip(1 - L / ref, 0, 1)                    # darkening
h = np.clip((L - ref) / max(1e-3, 1 - ref), 0, 1) * 0.7   # highlights
k = m * (1 - s) * (1 - h)                         # how much art shows
C = a[..., :3] * a[..., 3:4] * (1 - m[..., None]) + (m * h)[..., None]
ov_a = 1 - k
ov_rgb = np.where(ov_a[..., None] > 1e-4, C / np.maximum(ov_a[..., None], 1e-4), 0)
inside = np.zeros_like(m, bool)
inside[ly:ly1, lx:lx1] = True
ov = np.zeros((H, W, 4), np.float32)
ov[..., :3] = np.clip(ov_rgb, 0, 1)
ov[..., 3] = np.where(inside, ov_a, 0)
Image.fromarray((ov * 255 + 0.5).astype(np.uint8), 'RGBA').save(os.path.join(dst, 'over.png'), optimize=True)
A.save(os.path.join(dst, 'base.png'), optimize=True)
json.dump(dict(w=W, h=H, label=[lx, ly, lx1 - lx, ly1 - ly], ref=ref), open(os.path.join(dst, 'meta.json'), 'w'))
print(dst, W, H, (lx, ly, lx1 - lx, ly1 - ly), round(ref, 3))
