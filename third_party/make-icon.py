# -*- coding: utf-8 -*-
"""Draw Project1/Project1/shotcoil.ico.

The game ships no image assets, and the icon keeps that promise: it is authored
here as a 16x16 pixel grid and nearest-neighbour upscaled to the sizes Windows
asks for, so every size stays on the same pixel grid as the rest of the game's
art. Editing it means editing the ART grid below, not opening a paint program.

    python third_party/make-icon.py

Needs Pillow:  pip install pillow
"""

import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Project1', 'Project1', 'shotcoil.ico')

# The player: a pale box with stick eyes, mid-shot, muzzle flash at the corner.
#   . background   # white shell   C body   K eye ink   M muzzle flash
ART = [
    '................',
    '................',
    '..###########...',
    '..#CCCCCCCCC#...',
    '..#CCCCCCCCC#...',
    '..#CCKKCCKKC#...',
    '..#CCKKCCKKC#...',
    '..#CCKKCCKKC#...',
    '..#CCCCCCCCC#...',
    '..#CCCCCCCCC#...',
    '..###########.M.',
    '.............MMM',
    '..............M.',
    '................',
    '................',
    '................',
]

PALETTE = {
    '.': (0, 0, 0, 0),              # transparent - Windows supplies the backdrop
    '#': (238, 246, 255, 255),
    'C': (120, 230, 255, 255),
    'K': (18, 26, 46, 255),
    'M': (255, 220, 150, 255),
}

SIZES = [16, 32, 48, 64, 128, 256]


def main():
    n = len(ART)
    assert all(len(row) == n for row in ART), 'ART must be square'

    base = Image.new('RGBA', (n, n))
    base.putdata([PALETTE[ch] for row in ART for ch in row])

    # Save from the LARGEST frame. Pillow's ICO writer only emits the sizes it
    # can reach by downscaling the image it is given, so handing it the 16x16
    # silently produces a single-entry file - Explorer would then have nothing
    # but a 16px tile to stretch across a 256px thumbnail.
    #
    # Nearest everywhere keeps the pixel edges hard, which is the whole point.
    #
    # PNG frames, not BMP. A 256x256 BMP frame alone is 256KB of uncompressed
    # RGBA, which would eat most of what is left of the 1.44MB budget; the same
    # frame as PNG is a couple of KB because the art is flat colour. Windows has
    # read PNG-compressed icon frames since Vista.
    big = base.resize((max(SIZES), max(SIZES)), Image.NEAREST)
    big.save(OUT, format='ICO', sizes=[(s, s) for s in SIZES], bitmap_format='png')

    print('%s  (%d bytes)' % (OUT, os.path.getsize(OUT)))


if __name__ == '__main__':
    main()
