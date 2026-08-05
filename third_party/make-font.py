# -*- coding: utf-8 -*-
"""Bake Galmuri11 Regular/Bold into Project1/Project1/galmuri.h.

The contest budget is 1.44MB for the whole executable, and a full Korean TTF is
several megabytes on its own - so we ship only the glyphs main.c actually asks
for. Every codepoint that appears in the source (plus printable ASCII) is
collected, the two faces are subset down to exactly that set, and the remaining
bytes are emitted as C arrays that LoadFontFromMemory-style loading can use with
no asset file on disk.

Re-run this whenever new text is added to main.c:

    python third_party/make-font.py

Source fonts (OFL 1.1) are expected in third_party/fonts/ as Galmuri11.ttf and
Galmuri11-Bold.ttf. They are gitignored - the 8MB originals are build input, not
something the game ships. Fetch them from the Galmuri release zip:

    https://github.com/quiple/galmuri/releases

Needs fontTools:  pip install fonttools
"""

import io
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_C = os.path.join(ROOT, 'Project1', 'Project1', 'main.c')
OUT_H = os.path.join(ROOT, 'Project1', 'Project1', 'galmuri.h')
FONT_DIR = os.path.join(ROOT, 'third_party', 'fonts')

FACES = [
    ('GALMURI_REGULAR', 'Galmuri11.ttf'),
    ('GALMURI_BOLD', 'Galmuri11-Bold.ttf'),
]

# Tables stb_truetype (the rasteriser inside raylib) never reads.
DROP_TABLES = 'GSUB,GPOS,GDEF,DSIG,name,post,FFTM,gasp,VDMX,hdmx,LTSH,EBDT,EBLC,EBSC'


def wanted_codepoints():
    """Printable ASCII plus every non-ASCII character used in main.c."""
    text = io.open(MAIN_C, encoding='utf-8-sig').read()   # -sig: main.c carries a BOM
    cps = set(range(32, 127))
    cps.update(ord(ch) for ch in text if ord(ch) > 127)
    return sorted(cps)


def subset_face(path, codepoints):
    # recalcTimestamp=False keeps the source font's `head.modified` instead of
    # stamping "now" into it. Without this the output bytes differ on every run
    # even when nothing changed, which defeats the no-op check in main().
    font = TTFont(path, recalcTimestamp=False)
    options = subset.Options()
    options.drop_tables += DROP_TABLES.split(',')
    options.layout_features = []
    options.hinting = False
    options.glyph_names = False
    options.legacy_kern = False
    options.notdef_outline = True      # the box drawn for anything we missed
    options.recalc_bounds = True

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def c_array(name, data):
    out = ['#define %s_SIZE %d' % (name, len(data)),
           'static const unsigned char %s[%s_SIZE] = {' % (name, name)]
    for i in range(0, len(data), 16):
        out.append('    ' + ' '.join('0x%02x,' % b for b in data[i:i + 16]))
    out.append('};')
    return '\n'.join(out)


def main():
    codepoints = wanted_codepoints()
    missing = []
    blobs = []

    for name, filename in FACES:
        path = os.path.join(FONT_DIR, filename)
        if not os.path.exists(path):
            sys.exit('missing font: %s\n'
                     'grab it from https://github.com/quiple/galmuri/releases' % path)

        cmap = TTFont(path).getBestCmap()
        missing += [(filename, cp) for cp in codepoints if cp not in cmap]
        blobs.append((name, subset_face(path, codepoints)))

    if missing:
        for filename, cp in missing:
            print('WARNING: %s has no glyph for U+%04X (%s)'
                  % (filename, cp, chr(cp)), file=sys.stderr)

    parts = [
        '/******************************************************************************',
        '*   Galmuri11 - a Korean pixel font by Quiple, https://github.com/quiple/galmuri',
        '*   Licensed under the SIL Open Font License 1.1 (see third_party/fonts/OFL.txt)',
        '*',
        '*   GENERATED FILE - do not edit by hand.',
        '*   Regenerate with:  python third_party/make-font.py',
        '*',
        '*   Both faces are subset to the %d codepoints main.c actually draws.'
        % len(codepoints),
        '******************************************************************************/',
        '#ifndef GALMURI_H',
        '#define GALMURI_H',
        '',
    ]
    for name, data in blobs:
        parts.append(c_array(name, data))
        parts.append('')

    parts.append('#define GALMURI_CODEPOINT_COUNT %d' % len(codepoints))
    parts.append('static const int GALMURI_CODEPOINTS[GALMURI_CODEPOINT_COUNT] = {')
    for i in range(0, len(codepoints), 12):
        parts.append('    ' + ' '.join('%d,' % cp for cp in codepoints[i:i + 12]))
    parts.append('};')
    parts.append('')
    parts.append('#endif /* GALMURI_H */')

    text = '\n'.join(parts) + '\n'

    # Only touch the file when the bytes actually differ. This script runs as a
    # pre-build step, and rewriting an identical header every time would bump
    # its timestamp and force a full recompile of main.c on every build.
    if os.path.exists(OUT_H) and io.open(OUT_H, encoding='utf-8').read() == text:
        print('galmuri.h unchanged (%d codepoints)' % len(codepoints))
        return

    io.open(OUT_H, 'w', encoding='utf-8', newline='\n').write(text)

    for name, data in blobs:
        print('%-16s %7d bytes' % (name, len(data)))
    print('codepoints       %7d' % len(codepoints))
    print('-> %s' % OUT_H)


if __name__ == '__main__':
    main()
