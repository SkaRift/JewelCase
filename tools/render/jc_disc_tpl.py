"""Disc templates for Jewel Case, drawn to match real discs: the print runs almost to the
edge, a thin reflective silver rim around it, and a see-through plastic hub with its
stacking ring around the hole.

usage: python3 tools/render/jc_disc_tpl.py   (writes assets/media/disc_*/)
Then `make media`.
"""
import json
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'assets', 'media')
SS = 3

# name: (size px, rim tint, tint amount, mini)
DISCS = {
    'disc_black': (380, (40, 40, 46), 0.35, False),     # PS1: black data side shows at the edge
    'disc_blue': (380, (70, 110, 210), 0.30, False),    # PS2: blue
    'disc_silver': (380, (200, 204, 212), 0.0, False),
    'disc_white': (380, (230, 232, 238), 0.0, False),
    'disc_green': (380, (60, 150, 70), 0.25, False),
    # GameCube: an 8 cm disc (two thirds of a 12 cm one), bigger hub for its size
    'disc_mini': (254, (190, 194, 204), 0.0, True),
}


def smooth_band(r, a, b, aa):
    """1 inside a..b with soft edges aa wide"""
    return np.clip((r - a) / aa + 0.5, 0, 1) * np.clip((b - r) / aa + 0.5, 0, 1)


def make(size, tint, k, mini):
    n = size * SS
    c = n / 2
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    dx, dy = xx + 0.5 - c, yy + 0.5 - c
    r = np.hypot(dx, dy)
    th = np.arctan2(dy, dx)
    R = c - 1.5 * SS
    aa = 1.2 * SS

    rim_w = max(3.2 * SS, 0.022 * R)
    lab_out = R - rim_w
    hole = (0.19 if mini else 0.125) * R
    hub_out = (0.31 if mini else 0.245) * R
    lab_in = hub_out + max(2.0 * SS, 0.012 * R)
    stack = (0.255 if mini else 0.19) * R

    rgb = np.zeros((n, n, 3), np.float32)
    alpha = np.zeros((n, n), np.float32)

    # silver rim: a polished band that catches the light at two places, a faint rainbow
    s = 0.62 + 0.25 * np.cos(2 * th + 0.8) + 0.08 * np.cos(6 * th)
    rain = np.stack([np.sin(3 * th + p) for p in (0, 2.1, 4.2)], -1) * 0.05
    rim = np.clip(s[..., None] + rain, 0, 1) * 255
    rim = rim * (1 - k) + np.array(tint, np.float32) * k * (0.7 + 0.4 * s[..., None])
    # brighter toward the outer edge, a dark hairline where the print stops
    t = np.clip((r - lab_out) / rim_w, 0, 1)
    rim = rim * (0.82 + 0.25 * t[..., None])
    m_rim = smooth_band(r, lab_out, R, aa)
    rgb += rim * m_rim[..., None]
    alpha += m_rim
    edge = smooth_band(r, lab_out - 0.5 * SS, lab_out + 0.9 * SS, aa)
    rgb = rgb * (1 - 0.45 * edge[..., None])

    # the print area: covered by the label art, kept opaque here
    m_lab = smooth_band(r, lab_in, lab_out, aa)
    rgb += np.array((60, 62, 70), np.float32) * m_lab[..., None]
    alpha += m_lab

    # mirror ring between the hub and the print
    m_mir = smooth_band(r, hub_out, lab_in, aa * 0.8)
    mir = (0.70 + 0.22 * np.cos(2 * th + 0.8))[..., None] * np.array((235, 237, 242), np.float32)
    rgb += mir * m_mir[..., None]
    alpha += m_mir

    # clear plastic hub: mostly see-through, with the stacking ring and the hole's lip
    m_hub = smooth_band(r, hole, hub_out, aa)
    hub_a = 0.22 + 0.10 * np.cos(2 * th + 0.8)
    ring = smooth_band(r, stack - 1.4 * SS, stack + 1.4 * SS, aa)
    ring_d = smooth_band(r, stack - 3.4 * SS, stack - 1.4 * SS, aa)
    lip = smooth_band(r, hole, hole + 1.8 * SS, aa)
    outer = smooth_band(r, hub_out - 1.6 * SS, hub_out, aa)
    hub_rgb = np.full((n, n, 3), 236, np.float32)
    hub_alpha = hub_a + 0.45 * ring + 0.55 * lip + 0.35 * outer
    hub_rgb = hub_rgb * (1 - 0.55 * ring_d[..., None])
    hub_alpha = hub_alpha + 0.25 * ring_d
    hub_alpha = np.clip(hub_alpha, 0, 0.95) * m_hub
    rgb += hub_rgb * hub_alpha[..., None]
    alpha += hub_alpha

    alpha = np.clip(alpha, 0, 1)
    base_rgb = np.where(alpha[..., None] > 1e-4, rgb / np.maximum(alpha[..., None], 1e-4), 0)
    base = np.dstack([np.clip(base_rgb, 0, 255), alpha * 255]).astype(np.uint8)

    # overlay: a faint sheen across the print (labels are mostly matte) and its edges
    sheen = (np.clip(np.cos(2 * th + 0.8), 0, 1) ** 6) * 0.10
    ov_a = m_lab * sheen
    in_edge = smooth_band(r, lab_in, lab_in + 1.4 * SS, aa) * 0.35
    ov = np.zeros((n, n, 4), np.float32)
    ov[..., :3] = 255
    ov[..., 3] = ov_a
    ov[..., :3] = np.where(in_edge[..., None] > ov_a[..., None], 0, 255)
    ov[..., 3] = np.maximum(ov_a, in_edge)
    ov[..., 3] *= 255
    ov = ov.astype(np.uint8)

    def down(a):
        return Image.fromarray(a, 'RGBA').resize((size, size), Image.LANCZOS)

    return down(base), down(ov), int(round(lab_in / SS + 0.5)), int(lab_out / SS)


def main():
    for name, (size, tint, k, mini) in DISCS.items():
        base, ov, r_in, r_out = make(size, tint, k, mini)
        d = os.path.join(OUT, name)
        os.makedirs(d, exist_ok=True)
        base.save(os.path.join(d, 'base.png'), optimize=True)
        ov.save(os.path.join(d, 'over.png'), optimize=True)
        with open(os.path.join(d, 'meta.json'), 'w') as f:
            json.dump(dict(w=size, h=size, disc=[r_in, r_out]), f)
        print(name, size, r_in, r_out)


if __name__ == '__main__':
    main()
