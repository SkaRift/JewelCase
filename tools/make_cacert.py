#!/usr/bin/env python3
"""Turn third_party/cacert/cacert.pem (Mozilla's root certificates, as packaged by certifi,
MPL-2.0) into helper/cacert.h for the cover helper's HTTPS. Update the .pem, then run this."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
pem = open(os.path.join(ROOT, 'third_party', 'cacert', 'cacert.pem')).read()
lines = [l for l in pem.splitlines() if l and not l.startswith('#')]
out = ["/* Mozilla's root certificates (third_party/cacert/cacert.pem, MPL-2.0),",
       ' * so curl can check HTTPS servers itself. Made by tools/make_cacert.py. */',
       'static const char CACERT_PEM[] =']
out += ['  "%s\\n"' % l for l in lines]
out[-1] += ';'
open(os.path.join(ROOT, 'helper', 'cacert.h'), 'w').write('\n'.join(out) + '\n')
