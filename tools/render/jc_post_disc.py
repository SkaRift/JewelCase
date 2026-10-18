"""Disc template from the CD render: base.png, over.png, meta.json {"disc": [r_in, r_out]}.
usage: jc_post_disc.py <render dir> <out dir> <size px> <hub tint #rrggbb> [mini]
"""
import sys, os, json
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

src, dst, size, tint = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
os.makedirs(dst, exist_ok=True)
A = Image.open(os.path.join(src, 'beauty.png')).convert('RGBA')
Mi = Image.open(os.path.join(src, 'mask.png')).convert('RGBA')
bb = A.getbbox()
A = A.crop(bb).resize((size, size), Image.LANCZOS)
Mi = Mi.crop(bb).resize((size, size), Image.LANCZOS)
a = np.asarray(A, np.float32) / 255
m = np.asarray(Mi, np.float32)[..., 0] / 255 * (a[..., 3] > 0.01)
c = size / 2
yy, xx = np.mgrid[0:size, 0:size]
rr = np.hypot(xx + 0.5 - c, yy + 0.5 - c)
r_out = int(c - 1)
r_in = int(np.ceil(rr[(m > 0.5)].min()))

# tint the clear hub with the system's data-side colour
t = np.array([int(tint[i:i + 2], 16) for i in (1, 3, 5)], np.float32) / 255
hub = (rr < r_in + 1) & (a[..., 3] > 0.01)
a[hub, :3] = a[hub, :3] * (0.45 + 0.55 * t)

L = (a[..., :3] * [0.2126, 0.7152, 0.0722]).sum(-1)
ref = float(np.median(L[m > 0.95]))
s = np.clip(1 - L / ref, 0, 1)
h = np.clip((L - ref) / max(1e-3, 1 - ref), 0, 1) * 0.7
# glossy label: soft specular wedges like a printed disc
SS = 3
spec = Image.new('L', (size * SS, size * SS), 0)
sd = ImageDraw.Draw(spec)
for a0, a1, v in ((200, 232, 60), (20, 52, 42), (290, 300, 34), (110, 120, 26)):
    sd.pieslice([0, 0, size * SS, size * SS], a0, a1, fill=v)
spec = spec.filter(ImageFilter.GaussianBlur(10 * SS)).resize((size, size), Image.LANCZOS)
h = 1 - (1 - h) * (1 - np.asarray(spec, np.float32) / 255)
k = m * (1 - s) * (1 - h)
C = a[..., :3] * a[..., 3:4] * (1 - m[..., None]) + (m * h)[..., None]
ov_a = 1 - k
ov_rgb = np.where(ov_a[..., None] > 1e-4, C / np.maximum(ov_a[..., None], 1e-4), 0)
inside = (rr <= r_out + 0.5) & (rr >= r_in - 1.5)
ov = np.zeros((size, size, 4), np.float32)
ov[..., :3] = np.clip(ov_rgb, 0, 1)
ov[..., 3] = np.where(inside, ov_a, 0)
Image.fromarray((ov * 255 + 0.5).astype(np.uint8), 'RGBA').save(os.path.join(dst, 'over.png'), optimize=True)
Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA').save(os.path.join(dst, 'base.png'), optimize=True)
json.dump(dict(w=size, h=size, disc=[r_in, r_out]), open(os.path.join(dst, 'meta.json'), 'w'))
print(dst, size, r_in, r_out)
