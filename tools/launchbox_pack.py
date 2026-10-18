#!/usr/bin/env python3
"""Pack the parts of the LaunchBox Games Database that Jewel Case uses into the build.

LaunchBox publishes its whole database as one big download (Metadata.zip). Rather
than every console downloading all of it, this keeps only the systems Jewel Case
supports and only the fields it shows, one small compressed file per system:

  launchbox/<slug>.lbz   raw deflate of a tab-separated text file:
                         #jc-lb 1<TAB>platform<TAB>database date
                         name, year, genres, developer, publisher, players,
                         disc image file, box front image file, overview
                         (one game per line, "-" for no image)
  launchbox/VERSION      the database date, so consoles refresh their copy

The images themselves stay on images.launchbox-app.com; the console downloads
only the ones for the games it has.

usage: python3 tools/launchbox_pack.py Metadata.zip [--max-overview N]
Get Metadata.zip from https://gamesdb.launchbox-app.com/Metadata.zip, then
`make release` puts launchbox/ in the app.
"""
import argparse
import os
import re
import sys
import zipfile
import zlib
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'launchbox')


def platforms():
    """the LaunchBox platform names Jewel Case asks for, from main.c"""
    src = open(os.path.join(ROOT, 'src', 'gs', 'main.c'), encoding='utf-8').read()
    body = src[src.index('launchbox_platform(const System *s)'):]
    body = body[:body.index('};')]
    return sorted(set(re.findall(r'\{"[a-z0-9]+",\s*"([^"]+)"\}', body)))


def slug(p):
    """same as lb_slug() in helper/jc-lbdb.c and main.c"""
    return ''.join(c for c in p.lower() if c.isascii() and c.isalnum())


def region_rank(r):
    """same order as jc-lbdb.c: USA, World, Canada, then Europe, then Japan"""
    if not r:
        return 30
    if 'North America' in r or 'United States' in r:
        return 50
    if 'World' in r:
        return 40
    if 'Canada' in r:
        return 35
    if 'Europe' in r or 'United Kingdom' in r or 'Australia' in r:
        return 20
    if 'Japan' in r:
        return 10
    return 15


def clean(s):
    return ' '.join((s or '').split())


def cut(s, n):
    """at most n characters, ending on a whole sentence when one ends past the halfway point"""
    if len(s) <= n:
        return s
    head = s[:n]
    end = max(head.rfind('. '), head.rfind('! '), head.rfind('? '))
    if end >= n // 2:
        return head[:end + 1]
    return head.rsplit(' ', 1)[0] + '…'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('zip')
    ap.add_argument('--max-overview', type=int, default=500,
                    help='cut descriptions to this many characters (the details screen shows six lines, about that much)')
    a = ap.parse_args()

    want = {p.lower(): p for p in platforms()}
    zf = zipfile.ZipFile(a.zip)
    name = next(n for n in zf.namelist() if os.path.basename(n).lower() == 'metadata.xml')
    info = zf.getinfo(name)
    date = '%04d-%02d-%02d' % info.date_time[:3]
    print(f'{name}: {info.file_size >> 20} MB of XML, dated {date}; {len(want)} platforms wanted')

    games = {}     # id -> [platform, name, year, genres, dev, pub, players, overview]
    images = {}    # id -> {'Disc': (score, file), 'Box': (score, file)}
    with zf.open(name) as f:
        depth = 0
        for ev, el in ET.iterparse(f, ('start', 'end')):
            if ev == 'start':
                depth += 1
                continue
            depth -= 1
            if depth != 1:
                continue    # only the records right under <LaunchBox>, read whole
            tag = el.tag
            if tag == 'Game':
                plat = want.get((el.findtext('Platform') or '').lower())
                gid = el.findtext('DatabaseID')
                if plat and gid:
                    year = (el.findtext('ReleaseYear') or el.findtext('ReleaseDate') or '')[:4]
                    ov = clean(el.findtext('Overview'))
                    ov = cut(ov, a.max_overview)
                    games[gid] = [plat, clean(el.findtext('Name')), year, clean(el.findtext('Genres')),
                                  clean(el.findtext('Developer')), clean(el.findtext('Publisher')),
                                  clean(el.findtext('MaxPlayers')), ov]
                el.clear()
            elif tag == 'GameImage':
                gid, typ, fn = el.findtext('DatabaseID'), el.findtext('Type') or '', el.findtext('FileName') or ''
                if gid and fn and '\t' not in fn and len(fn) < 64:
                    score = region_rank(el.findtext('Region') or '')
                    kind = None
                    if typ == 'Disc':
                        kind = 'Disc'
                    elif typ == 'Box - Front':
                        kind = 'Box'
                    elif typ == 'Box - Front - Reconstructed':
                        kind, score = 'Box', score - 100
                    if kind:
                        d = images.setdefault(gid, {})
                        if kind not in d or score > d[kind][0]:
                            d[kind] = (score, fn)
                el.clear()
            else:
                el.clear()

    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.endswith('.lbz'):
            os.remove(os.path.join(OUT, old))
    by_plat = {}
    for gid, g in games.items():
        im = images.get(gid, {})
        by_plat.setdefault(g[0], []).append(g[1:7] + [im.get('Disc', (0, '-'))[1], im.get('Box', (0, '-'))[1], g[7]])
    total = 0
    for plat in sorted(by_plat):
        rows = sorted(by_plat[plat], key=lambda r: r[0].lower())
        text = f'#jc-lb 1\t{plat}\t{date}\n' + ''.join('\t'.join(r) + '\n' for r in rows if r[0])
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        data = c.compress(text.encode('utf-8')) + c.flush()
        with open(os.path.join(OUT, slug(plat) + '.lbz'), 'wb') as f:
            f.write(data)
        total += len(data)
        boxes = sum(r[7] != '-' for r in rows)
        discs = sum(r[6] != '-' for r in rows)
        print(f'  {plat:40s} {len(rows):6d} games {boxes:6d} boxes {discs:5d} discs {len(data) / 1024:8.0f} KB')
    with open(os.path.join(OUT, 'VERSION'), 'w') as f:
        f.write(date + '\n')
    missing = sorted(set(want.values()) - set(by_plat))
    if missing:
        print('not in the database:', ', '.join(missing))
    print(f'{total / 1048576:.1f} MB in {OUT}')


if __name__ == '__main__':
    sys.exit(main())
