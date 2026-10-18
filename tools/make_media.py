#!/usr/bin/env python3
"""Jewel Case media generator.

Draws the blank cartridge / card / disc / case templates, the small UI icons,
the glow and shadow sprites and the bokeh light map, then writes them into
src/gs/media.h as embedded PNGs plus a table of where each template's art goes.

All shapes are original, generic designs (no logos, no trademarks).

    python3 tools/make_media.py [--preview out_dir]
"""
import io
import math
import json
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SS = 3  # supersampling
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def hexc(h, a=255):
    h = h.lstrip('#')
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def canvas(w, h):
    return Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))


def down(img):
    w, h = img.size
    return img.resize((w // SS, h // SS), Image.LANCZOS)


def S(v):
    return int(round(v * SS))


def vgrad(w, h, top, bottom):
    """vertical gradient RGBA image (supersampled size)"""
    t = np.linspace(0, 1, h * SS)[:, None]
    a = np.array(top, dtype=np.float32)
    b = np.array(bottom, dtype=np.float32)
    rows = a[None, :] * (1 - t) + b[None, :] * t
    arr = np.repeat(rows[:, None, :], w * SS, axis=1)
    return Image.fromarray(arr.astype(np.uint8), 'RGBA')


def shape_mask(w, h, draw_fn):
    m = Image.new('L', (w * SS, h * SS), 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def fill_with(mask, img):
    out = Image.new('RGBA', mask.size, (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def edge_light(mask, strength=70, dark=90):
    """top/left rim highlight and bottom/right rim shade from the shape mask"""
    m = np.asarray(mask, dtype=np.float32) / 255.0
    sh = S(2.2)
    up = np.zeros_like(m)
    up[sh:, :] = m[:-sh, :]
    dn = np.zeros_like(m)
    dn[:-sh, :] = m[sh:, :]
    hi = np.clip(m - up, 0, 1)        # top edge
    lo = np.clip(m - dn, 0, 1)        # bottom edge
    arr = np.zeros(m.shape + (4,), dtype=np.float32)
    arr[..., 0:3] = 255
    arr[..., 3] = hi * strength
    hi_img = Image.fromarray(arr.astype(np.uint8), 'RGBA')
    arr2 = np.zeros(m.shape + (4,), dtype=np.float32)
    arr2[..., 3] = lo * dark
    lo_img = Image.fromarray(arr2.astype(np.uint8), 'RGBA')
    return hi_img, lo_img


def rrect(d, box, r, fill):
    d.rounded_rectangle([S(box[0]), S(box[1]), S(box[2]), S(box[3])], radius=S(r), fill=fill)


def label_recess(img, label, r=6):
    """dark border around the label area, and a dark label bed (blended onto img)"""
    x, y, w, h = label
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rrect(d, (x - 6, y - 6, x + w + 6, y + h + 6), r + 5, (0, 0, 0, 70))
    rrect(d, (x - 3, y - 3, x + w + 3, y + h + 3), r + 3, (0, 0, 0, 90))
    rrect(d, (x, y, x + w, y + h), r, (30, 30, 34, 255))
    img.alpha_composite(Image.alpha_composite(Image.new('RGBA', img.size, (0, 0, 0, 0)), layer))
    # outer ring is translucent black over the body: composite properly
    return img


def gloss(w, h, label, r=6, amount=1.0):
    """overlay drawn on top of the art: soft diagonal sheen + inner label shade"""
    x, y, lw, lh = label
    ov = canvas(w, h)
    # diagonal sheen limited to the label
    sheen = Image.new('L', ov.size, 0)
    sd = ImageDraw.Draw(sheen)
    sd.polygon([(S(x), S(y)), (S(x + lw * 0.55), S(y)), (S(x + lw * 0.25), S(y + lh)), (S(x), S(y + lh))],
               fill=int(38 * amount))
    sd.polygon([(S(x + lw * 0.62), S(y)), (S(x + lw * 0.70), S(y)), (S(x + lw * 0.40), S(y + lh)),
                (S(x + lw * 0.32), S(y + lh))], fill=int(22 * amount))
    sheen = sheen.filter(ImageFilter.GaussianBlur(S(6)))
    lm = shape_mask(w, h, lambda d: rrect(d, (x, y, x + lw, y + lh), r, 255))
    sheen = Image.fromarray((np.asarray(sheen, np.float32) * np.asarray(lm, np.float32) / 255).astype(np.uint8))
    white = Image.new('RGBA', ov.size, (255, 255, 255, 255))
    ov.paste(white, (0, 0), sheen)
    # inner shade along the label's top edge (art sits slightly below the shell)
    sh = Image.new('L', ov.size, 0)
    shd = ImageDraw.Draw(sh)
    shd.rounded_rectangle([S(x), S(y), S(x + lw), S(y + 6)], radius=S(3), fill=70)
    sh = sh.filter(ImageFilter.GaussianBlur(S(3)))
    sh = Image.fromarray((np.asarray(sh, np.float32) * np.asarray(lm, np.float32) / 255).astype(np.uint8))
    black = Image.new('RGBA', ov.size, (0, 0, 0, 255))
    ov.paste(black, (0, 0), sh)
    return down(ov)


def finish(body_mask, w, h, top, bottom, extras=None, rim=70, shade=90):
    body = fill_with(body_mask, vgrad(w, h, top, bottom))
    if extras:
        layer = Image.new('RGBA', body.size, (0, 0, 0, 0))
        extras(layer)
        body = Image.alpha_composite(body, layer)
    hi, lo = edge_light(body_mask, rim, shade)
    body = Image.alpha_composite(body, hi)
    body = Image.alpha_composite(body, lo)
    # keep the extras inside the shape
    a = np.asarray(body).copy()
    a[..., 3] = np.minimum(a[..., 3], np.asarray(body_mask))
    return Image.fromarray(a, 'RGBA')


def ridges(img, x0, x1, y0, y1, step, horizontal=True, light=(255, 255, 255, 40), dark=(0, 0, 0, 60)):
    d = ImageDraw.Draw(img)
    if horizontal:
        y = y0
        while y < y1:
            d.rectangle([S(x0), S(y), S(x1), S(y + 1.4)], fill=dark)
            d.rectangle([S(x0), S(y + 1.4), S(x1), S(y + 2.4)], fill=light)
            y += step
    else:
        x = x0
        while x < x1:
            d.rectangle([S(x), S(y0), S(x + 1.4), S(y1)], fill=dark)
            d.rectangle([S(x + 1.4), S(y0), S(x + 2.4), S(y1)], fill=light)
            x += step


# ---------------------------------------------------------------------------
# cartridges and cards. Each returns (base, overlay, label rect, radius)
# ---------------------------------------------------------------------------

def tpl_tall(w=300, h=340, top='#a9a9ae', bottom='#8b8b91'):
    """tall grey cartridge with grip ridges at the bottom"""
    label = (36, 34, 228, 214)

    def m(d):
        rrect(d, (0, 0, w, h - 14), 10, 255)
        rrect(d, (14, h - 30, w - 14, h), 6, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        ridges(img, 30, w - 30, h - 74, h - 22, 7)
        d = ImageDraw.Draw(img)
        d.rectangle([S(0), S(h - 92), S(w), S(h - 90)], fill=(0, 0, 0, 50))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex)
    base_i = base.copy()
    label_recess(base_i, label)
    return down(base_i), gloss(w, h, label), label, 6


def tpl_wide(w=440, h=300, top='#c6c3ce', bottom='#a6a3b1'):
    """wide light cartridge with side grooves"""
    label = (84, 40, 272, 182)

    def m(d):
        rrect(d, (0, 0, w, h), 28, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        d = ImageDraw.Draw(img)
        for x in (26, w - 54):
            rrect(d, (x, 24, x + 28, h - 24), 10, (0, 0, 0, 34))
            ridges(img, x + 4, x + 24, 34, h - 34, 9, True, (255, 255, 255, 30), (0, 0, 0, 40))
        rrect(d, (70, h - 52, w - 70, h - 34), 8, (0, 0, 0, 30))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex)
    label_recess(base, label)
    return down(base), gloss(w, h, label), label, 6


def tpl_chunky(w=400, h=330, top='#62646c', bottom='#45474e'):
    """chunky dark cartridge, rounded top, slightly narrower bottom"""
    label = (64, 34, 272, 196)

    def m(d):
        d.polygon([(S(10), S(40)), (S(w - 10), S(40)), (S(w - 34), S(h)), (S(34), S(h))], fill=255)
        rrect(d, (0, 0, w, 120), 40, 255)
        rrect(d, (26, h - 40, w - 26, h), 10, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        d = ImageDraw.Draw(img)
        ridges(img, 70, w - 70, h - 78, h - 30, 8, True, (255, 255, 255, 26), (0, 0, 0, 70))
        d.rectangle([S(40), S(h - 92), S(w - 40), S(h - 89)], fill=(0, 0, 0, 60))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex, rim=50, shade=110)
    label_recess(base, label)
    return down(base), gloss(w, h, label), label, 6


def tpl_small(w=270, h=300, top='#bdbcc1', bottom='#9d9ca3'):
    """small handheld cartridge with a cut corner and grip ridges on top"""
    label = (32, 94, 206, 170)

    def m(d):
        d.polygon([(S(10), 0), (S(w - 40), 0), (S(w), S(40)), (S(w), S(h - 10)), (S(w - 10), S(h)),
                   (S(10), S(h)), (0, S(h - 10)), (0, S(10))], fill=255)

    mask = shape_mask(w, h, m)

    def ex(img):
        ridges(img, 24, w - 60, 16, 70, 7)
        d = ImageDraw.Draw(img)
        d.rectangle([S(0), S(78), S(w), S(80)], fill=(0, 0, 0, 40))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex)
    label_recess(base, label)
    return down(base), gloss(w, h, label), label, 6


def tpl_slim(w=400, h=250, top='#41434c', bottom='#2a2c33'):
    """slim wide dark cartridge"""
    label = (70, 28, 260, 166)

    def m(d):
        rrect(d, (0, 0, w, h - 20), 22, 255)
        rrect(d, (40, h - 46, w - 40, h), 12, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        d = ImageDraw.Draw(img)
        for x in (18, w - 46):
            ridges(img, x, x + 28, 30, h - 70, 8, True, (255, 255, 255, 20), (0, 0, 0, 70))
        d.rectangle([S(40), S(h - 36), S(w - 40), S(h - 34)], fill=(0, 0, 0, 70))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex, rim=45, shade=110)
    label_recess(base, label)
    return down(base), gloss(w, h, label), label, 6


def tpl_card(w=270, h=310, top='#e3e5ea', bottom='#c6c9d1'):
    """thin game card with a small tab"""
    label = (28, 58, 214, 218)

    def m(d):
        d.polygon([(S(14), 0), (S(w - 14), 0), (S(w), S(14)), (S(w), S(h)), (0, S(h)), (0, S(40)),
                   (S(14), S(26))], fill=255)

    mask = shape_mask(w, h, m)

    def ex(img):
        d = ImageDraw.Draw(img)
        rrect(d, (w - 80, 12, w - 24, 36), 6, (0, 0, 0, 26))
        d.rectangle([S(0), S(h - 22), S(w), S(h)], fill=(0, 0, 0, 22))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex, rim=90, shade=60)
    label_recess(base, label, 4)
    return down(base), gloss(w, h, label, 4), label, 4


def tpl_minicard(w=220, h=320, top='#3a3c44', bottom='#25272d'):
    """small dark card with a coloured top strip"""
    label = (20, 50, 180, 246)

    def m(d):
        rrect(d, (0, 0, w, h), 14, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([0, 0, S(w), S(34)], radius=S(14), fill=(160, 166, 180, 120))
        d.rectangle([0, S(18), S(w), S(34)], fill=(160, 166, 180, 120))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex, rim=50, shade=90)
    label_recess(base, label, 6)
    return down(base), gloss(w, h, label), label, 6


def tpl_black(w=340, h=370, top='#34353b', bottom='#1d1e22'):
    """black cartridge with a grip top"""
    label = (40, 70, 260, 238)

    def m(d):
        rrect(d, (0, 0, w, h), 12, 255)

    mask = shape_mask(w, h, m)

    def ex(img):
        ridges(img, 28, w - 28, 14, 54, 7, True, (255, 255, 255, 22), (0, 0, 0, 70))

    base = finish(mask, w, h, hexc(top), hexc(bottom), ex, rim=40, shade=100)
    label_recess(base, label)
    return down(base), gloss(w, h, label), label, 6


def tpl_case(w=290, h=380):
    """generic clear keep case: the art fills it"""
    label = (16, 12, 262, 356)

    def m(d):
        rrect(d, (0, 0, w, h), 10, 255)

    mask = shape_mask(w, h, m)
    base = fill_with(mask, vgrad(w, h, (36, 38, 46, 255), (22, 23, 28, 255)))
    d = ImageDraw.Draw(base)
    rrect(d, (label[0], label[1], label[0] + label[2], label[1] + label[3]), 4, (30, 30, 34, 255))
    ov = canvas(w, h)
    od = ImageDraw.Draw(ov)
    # clear plastic: spine, border, sheen
    od.rectangle([S(2), S(4), S(12), S(h - 4)], fill=(255, 255, 255, 30))
    od.rounded_rectangle([S(1), S(1), S(w - 1), S(h - 1)], radius=S(10), outline=(255, 255, 255, 70), width=S(2))
    sheen = Image.new('L', ov.size, 0)
    ImageDraw.Draw(sheen).polygon([(S(16), S(12)), (S(150), S(12)), (S(60), S(368)), (S(16), S(368))], fill=34)
    sheen = sheen.filter(ImageFilter.GaussianBlur(S(8)))
    ov.paste(Image.new('RGBA', ov.size, (255, 255, 255, 255)), (0, 0), sheen)
    return down(base), down(ov), label, 4


def tpl_disc(size=380, data='#1c1d22', ring='#c9ccd4', mini=False):
    """optical disc: art printed on the label side, a clear hub and a rim.
    Label area is the annulus r_in..r_out (returned instead of a rect)."""
    w = h = size
    c = size / 2
    r_out = c - 6
    r_in = size * (0.115 if not mini else 0.13)
    base = canvas(w, h)
    d = ImageDraw.Draw(base)
    # rim (data side colour shows at the edge)
    d.ellipse([S(c - c + 1), S(1), S(size - 1), S(size - 1)], fill=hexc(data))
    d.ellipse([S(c - r_out), S(c - r_out), S(c + r_out), S(c + r_out)], fill=(30, 30, 34, 255))
    # clear hub
    hub = r_in
    d.ellipse([S(c - hub), S(c - hub), S(c + hub), S(c + hub)], fill=hexc(ring, 255))
    d.ellipse([S(c - hub * 0.78), S(c - hub * 0.78), S(c + hub * 0.78), S(c + hub * 0.78)], fill=(170, 176, 190, 255))
    d.ellipse([S(c - hub * 0.42), S(c - hub * 0.42), S(c + hub * 0.42), S(c + hub * 0.42)], fill=(0, 0, 0, 0))
    ov = canvas(w, h)
    od = ImageDraw.Draw(ov)
    # specular: two soft wedges across the label
    spec = Image.new('L', ov.size, 0)
    sd = ImageDraw.Draw(spec)
    for a0, a1, v in ((200, 232, 70), (20, 52, 48), (290, 300, 40), (110, 120, 30)):
        sd.pieslice([S(c - r_out), S(c - r_out), S(c + r_out), S(c + r_out)], a0, a1, fill=v)
    spec = spec.filter(ImageFilter.GaussianBlur(S(10)))
    ann = Image.new('L', ov.size, 0)
    ad = ImageDraw.Draw(ann)
    ad.ellipse([S(c - r_out), S(c - r_out), S(c + r_out), S(c + r_out)], fill=255)
    ad.ellipse([S(c - r_in), S(c - r_in), S(c + r_in), S(c + r_in)], fill=0)
    spec = Image.fromarray((np.asarray(spec, np.float32) * np.asarray(ann, np.float32) / 255).astype(np.uint8))
    ov.paste(Image.new('RGBA', ov.size, (255, 255, 255, 255)), (0, 0), spec)
    # rings: label edge and the hub's stacking ring
    od.ellipse([S(c - r_out), S(c - r_out), S(c + r_out), S(c + r_out)], outline=(255, 255, 255, 60), width=S(1.5))
    od.ellipse([S(c - r_in), S(c - r_in), S(c + r_in), S(c + r_in)], outline=(0, 0, 0, 120), width=S(3))
    od.ellipse([S(c - r_in - 14), S(c - r_in - 14), S(c + r_in + 14), S(c + r_in + 14)],
               outline=(255, 255, 255, 50), width=S(1.5))
    od.ellipse([S(1), S(1), S(size - 1), S(size - 1)], outline=(255, 255, 255, 90), width=S(2))
    return down(base), down(ov), (int(r_in), int(r_out)), 0


# kinds: 0 = rect label, 1 = disc annulus
TEMPLATES = [
    # name, builder, kind
    ('tall', lambda: tpl_tall(), 0),
    ('wide', lambda: tpl_wide(), 0),
    ('chunky', lambda: tpl_chunky(), 0),
    ('small', lambda: tpl_small(), 0),
    ('small_dark', lambda: tpl_small(top='#55575f', bottom='#3a3c43'), 0),
    ('slim', lambda: tpl_slim(), 0),
    ('card', lambda: tpl_card(), 0),
    ('card_white', lambda: tpl_card(top='#f4f4f6', bottom='#d9dbe0'), 0),
    ('minicard', lambda: tpl_minicard(), 0),
    ('black', lambda: tpl_black(), 0),
    ('case', lambda: tpl_case(), 0),
    ('disc_black', lambda: tpl_disc(data='#121216', ring='#b9bdc8'), 1),
    ('disc_blue', lambda: tpl_disc(data='#1d3fa0', ring='#9fb4ee'), 1),
    ('disc_silver', lambda: tpl_disc(data='#b8bcc6', ring='#e2e5ec'), 1),
    ('disc_white', lambda: tpl_disc(data='#e8e9ee', ring='#f4f5f8'), 1),
    ('disc_green', lambda: tpl_disc(data='#2f7d33', ring='#b8d8b9'), 1),
    ('disc_mini', lambda: tpl_disc(size=300, data='#5a46b8', ring='#c4b9f0', mini=True), 1),
]

# ---------------------------------------------------------------------------
# icons: 64x64 white glyphs
# ---------------------------------------------------------------------------

def icon(draw_fn, size=64):
    im = Image.new('L', (size * SS, size * SS), 0)
    draw_fn(ImageDraw.Draw(im), lambda v: int(round(v * SS)))
    im = im.resize((size, size), Image.LANCZOS)
    out = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    out.putalpha(im)
    return out


def ic_clock(d, s):
    d.ellipse([s(8), s(8), s(56), s(56)], outline=255, width=s(5))
    d.line([s(32), s(18), s(32), s(33)], fill=255, width=s(5))
    d.line([s(32), s(33), s(43), s(40)], fill=255, width=s(5))


def ic_star(d, s):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 26 if i % 2 == 0 else 11
        pts.append((s(32 + r * math.cos(a)), s(34 + r * math.sin(a))))
    d.polygon(pts, fill=255)


def ic_cart_wide(d, s):
    d.rounded_rectangle([s(6), s(14), s(58), s(50)], radius=s(7), outline=255, width=s(4))
    d.rounded_rectangle([s(18), s(22), s(46), s(40)], radius=s(3), fill=255)


def ic_cart_tall(d, s):
    d.rounded_rectangle([s(14), s(6), s(50), s(56)], radius=s(5), outline=255, width=s(4))
    d.rounded_rectangle([s(21), s(13), s(43), s(36)], radius=s(3), fill=255)
    for y in (43, 48):
        d.line([s(22), s(y), s(42), s(y)], fill=255, width=s(2))


def ic_handheld(d, s):
    d.rounded_rectangle([s(16), s(4), s(48), s(60)], radius=s(6), outline=255, width=s(4))
    d.rectangle([s(22), s(11), s(42), s(29)], fill=255)
    d.line([s(22), s(43), s(30), s(43)], fill=255, width=s(3))
    d.line([s(26), s(39), s(26), s(47)], fill=255, width=s(3))
    d.ellipse([s(36), s(39), s(42), s(45)], fill=255)


def ic_handheld_wide(d, s):
    d.rounded_rectangle([s(4), s(18), s(60), s(46)], radius=s(12), outline=255, width=s(4))
    d.rectangle([s(22), s(24), s(42), s(40)], fill=255)
    d.line([s(10), s(32), s(17), s(32)], fill=255, width=s(3))
    d.ellipse([s(47), s(29), s(53), s(35)], fill=255)


def ic_dual(d, s):
    d.rounded_rectangle([s(12), s(4), s(52), s(30)], radius=s(4), outline=255, width=s(4))
    d.rounded_rectangle([s(12), s(34), s(52), s(60)], radius=s(4), outline=255, width=s(4))
    d.rectangle([s(20), s(11), s(44), s(23)], fill=255)
    d.rectangle([s(20), s(41), s(44), s(53)], fill=255)


def ic_disc(d, s):
    d.ellipse([s(6), s(6), s(58), s(58)], outline=255, width=s(4))
    d.ellipse([s(25), s(25), s(39), s(39)], outline=255, width=s(4))
    d.arc([s(13), s(13), s(51), s(51)], 200, 250, fill=255, width=s(3))


def ic_card(d, s):
    d.polygon([(s(18), s(6)), (s(46), s(6)), (s(50), s(10)), (s(50), s(58)), (s(14), s(58)), (s(14), s(16)),
               (s(18), s(12))], outline=255, width=s(4))
    d.rectangle([s(21), s(20), s(43), s(44)], fill=255)


def ic_tablet(d, s):
    d.rounded_rectangle([s(4), s(14), s(60), s(50)], radius=s(9), outline=255, width=s(4))
    d.rectangle([s(18), s(21), s(46), s(43)], fill=255)


def ic_hybrid(d, s):
    d.rounded_rectangle([s(2), s(16), s(16), s(48)], radius=s(7), fill=255)
    d.rounded_rectangle([s(48), s(16), s(62), s(48)], radius=s(7), fill=255)
    d.rectangle([s(19), s(16), s(45), s(48)], outline=255, width=s(4))


def ic_stick(d, s):
    d.rounded_rectangle([s(6), s(38), s(58), s(56)], radius=s(6), outline=255, width=s(4))
    d.line([s(24), s(38), s(24), s(18)], fill=255, width=s(4))
    d.ellipse([s(15), s(6), s(33), s(24)], fill=255)
    d.ellipse([s(38), s(43), s(46), s(51)], fill=255)


def ic_computer(d, s):
    d.rounded_rectangle([s(6), s(8), s(58), s(44)], radius=s(4), outline=255, width=s(4))
    d.rectangle([s(26), s(44), s(38), s(52)], fill=255)
    d.rectangle([s(16), s(52), s(48), s(57)], fill=255)


def ic_pad(d, s):
    d.rounded_rectangle([s(4), s(16), s(60), s(46)], radius=s(14), outline=255, width=s(4))
    d.line([s(14), s(31), s(26), s(31)], fill=255, width=s(4))
    d.line([s(20), s(25), s(20), s(37)], fill=255, width=s(4))
    d.ellipse([s(40), s(23), s(47), s(30)], fill=255)
    d.ellipse([s(46), s(31), s(53), s(38)], fill=255)


def ic_stack(d, s):
    d.rounded_rectangle([s(10), s(8), s(54), s(24)], radius=s(4), outline=255, width=s(4))
    d.rounded_rectangle([s(10), s(26), s(54), s(42)], radius=s(4), outline=255, width=s(4))
    d.rounded_rectangle([s(10), s(44), s(54), s(60)], radius=s(4), outline=255, width=s(4))


def ic_chip(d, s):
    d.rounded_rectangle([s(14), s(14), s(50), s(50)], radius=s(5), outline=255, width=s(4))
    d.rectangle([s(24), s(24), s(40), s(40)], fill=255)
    for i in range(3):
        y = 22 + i * 10
        d.line([s(5), s(y), s(14), s(y)], fill=255, width=s(3))
        d.line([s(50), s(y), s(59), s(y)], fill=255, width=s(3))


def ic_gear(d, s):
    for i in range(8):
        a = i * math.pi / 4
        d.line([s(32 + 16 * math.cos(a)), s(32 + 16 * math.sin(a)), s(32 + 27 * math.cos(a)),
                s(32 + 27 * math.sin(a))], fill=255, width=s(9))
    d.ellipse([s(12), s(12), s(52), s(52)], fill=255)
    d.ellipse([s(24), s(24), s(40), s(40)], fill=0)


def ic_download(d, s):
    d.line([s(32), s(8), s(32), s(40)], fill=255, width=s(5))
    d.polygon([(s(18), s(28)), (s(46), s(28)), (s(32), s(44))], fill=255)
    d.line([s(10), s(54), s(54), s(54)], fill=255, width=s(5))


ICONS = [
    ('clock', ic_clock), ('star', ic_star), ('cart_wide', ic_cart_wide), ('cart_tall', ic_cart_tall),
    ('handheld', ic_handheld), ('handheld_wide', ic_handheld_wide), ('dual', ic_dual), ('disc', ic_disc),
    ('card', ic_card), ('tablet', ic_tablet), ('hybrid', ic_hybrid), ('stick', ic_stick),
    ('computer', ic_computer), ('pad', ic_pad), ('stack', ic_stack), ('chip', ic_chip), ('gear', ic_gear),
    ('download', ic_download),
]

# ---------------------------------------------------------------------------
# sprites
# ---------------------------------------------------------------------------

def glow_sprite(n=256):
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    r = np.sqrt((x - n / 2 + 0.5) ** 2 + (y - n / 2 + 0.5) ** 2) / (n / 2)
    a = np.clip(np.exp(-(r ** 2) * 4.2) - 0.012, 0, 1) * 255
    im = np.zeros((n, n, 4), np.uint8)
    im[..., 0:3] = 255
    im[..., 3] = a.astype(np.uint8)
    return Image.fromarray(im, 'RGBA')


def shadow_sprite(w=256, h=64):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = np.sqrt(((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2)
    a = np.clip(1 - r, 0, 1) ** 1.6 * 255
    im = np.zeros((h, w, 4), np.uint8)
    im[..., 3] = a.astype(np.uint8)
    return Image.fromarray(im, 'RGBA')


def bokeh_map(w=960, h=540, seed=7):
    """greyscale light map: soft bokeh circles (mostly low and to the sides)
    plus faint diagonal rain streaks. Tinted with the system colour at runtime."""
    rnd = random.Random(seed)
    acc = np.zeros((h, w), np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    for _ in range(70):
        cx = rnd.uniform(0, w)
        # more lights in the lower half, fewer behind the carousel centre
        cy = h * (0.35 + 0.65 * rnd.random() ** 0.7)
        r = rnd.uniform(10, 46)
        v = rnd.uniform(0.18, 0.55) * (0.55 if abs(cx - w / 2) < w * 0.18 and cy < h * 0.75 else 1)
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
        disk = np.clip(1.15 - d, 0, 1) ** 0.9
        disk *= (0.75 + 0.25 * np.clip(d, 0, 1))  # slightly brighter rim
        acc += disk * v
    img = Image.fromarray(np.clip(acc * 255, 0, 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(3))
    streaks = Image.new('L', (w, h), 0)
    sd = ImageDraw.Draw(streaks)
    for _ in range(140):
        x = rnd.uniform(-60, w)
        y = rnd.uniform(-20, h * 0.8)
        ln = rnd.uniform(14, 40)
        sd.line([(x, y), (x + ln * 0.22, y + ln)], fill=int(rnd.uniform(18, 46)), width=1)
    streaks = streaks.filter(ImageFilter.GaussianBlur(0.6))
    a = np.asarray(img, np.float32) + np.asarray(streaks, np.float32)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'L')


# ---------------------------------------------------------------------------

def png_bytes(im):
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=True)
    return b.getvalue()


def c_array(name, data):
    lines = [f'static const unsigned char {name}[{len(data)}] = {{']
    for i in range(0, len(data), 20):
        lines.append('    ' + ','.join(str(x) for x in data[i:i + 20]) + ',')
    lines.append('};')
    return '\n'.join(lines)


RENDERED = os.path.join(ROOT, 'assets', 'media')


def rendered(name):
    """A template rendered from a 3D model (tools/render/), if there is one:
    assets/media/<name>/base.png, over.png and meta.json (label rect, or disc radii)."""
    d = os.path.join(RENDERED, name)
    if not os.path.exists(os.path.join(d, 'meta.json')):
        return None
    with open(os.path.join(d, 'meta.json')) as f:
        meta = json.load(f)
    base = Image.open(os.path.join(d, 'base.png')).convert('RGBA')
    ov = Image.open(os.path.join(d, 'over.png')).convert('RGBA')
    label = tuple(meta['disc']) if 'disc' in meta else tuple(meta['label'])
    return base, ov, label, 0


def main():
    preview = None
    if '--preview' in sys.argv:
        preview = sys.argv[sys.argv.index('--preview') + 1]
        os.makedirs(preview, exist_ok=True)

    out = ['/* Jewel Case media: generated by tools/make_media.py. Do not edit. */', '#pragma once', '']
    table = []
    for i, (name, fn, kind) in enumerate(TEMPLATES):
        base, ov, label, radius = rendered(name) or fn()
        out.append(c_array(f'tpl_{name}_base', png_bytes(base)))
        out.append(c_array(f'tpl_{name}_over', png_bytes(ov)))
        if kind == 0:
            lx, ly, lw, lh = label
        else:
            lx, ly, lw, lh = label[0], label[1], 0, 0
        table.append(f'    {{"{name}", {kind}, {base.size[0]}, {base.size[1]}, {lx}, {ly}, {lw}, {lh}, {radius}, '
                     f'tpl_{name}_base, sizeof(tpl_{name}_base), tpl_{name}_over, sizeof(tpl_{name}_over)}},')
        if preview:
            Image.alpha_composite(base, ov).save(os.path.join(preview, f'tpl_{name}.png'))
            base.save(os.path.join(preview, f'tpl_{name}_base.png'))
    out.append('')
    out.append('typedef struct {\n    const char *name;\n    int kind;          /* 0 rect label, 1 disc */\n'
               '    int w, h;\n    int lx, ly, lw, lh;  /* disc: lx = inner radius, ly = outer radius */\n'
               '    int radius;\n    const unsigned char *base; unsigned base_len;\n'
               '    const unsigned char *over; unsigned over_len;\n} MediaTpl;')
    out.append('static const MediaTpl MEDIA_TPL[] = {')
    out.extend(table)
    out.append('};')
    out.append(f'#define MEDIA_TPL_COUNT {len(TEMPLATES)}')
    out.append('')

    # icon sheet: one row of 64x64 icons
    sheet = Image.new('RGBA', (64 * len(ICONS), 64), (255, 255, 255, 0))
    for i, (name, fn) in enumerate(ICONS):
        sheet.paste(icon(fn), (i * 64, 0))
        out.append(f'#define ICON_{name.upper()} {i}')
    out.append(c_array('icon_sheet_png', png_bytes(sheet)))
    out.append(c_array('glow_png', png_bytes(glow_sprite())))
    out.append(c_array('shadow_png', png_bytes(shadow_sprite())))
    bk = bokeh_map()
    out.append(c_array('bokeh_png', png_bytes(bk)))
    if preview:
        bg = Image.new('RGBA', sheet.size, (20, 22, 34, 255))
        Image.alpha_composite(bg, sheet).save(os.path.join(preview, 'icons.png'))
        bk.save(os.path.join(preview, 'bokeh.png'))

    with open(os.path.join(ROOT, 'src', 'gs', 'media.h'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote src/gs/media.h:', len(TEMPLATES), 'templates,', len(ICONS), 'icons')


if __name__ == '__main__':
    main()
