#!/usr/bin/env python3
"""Generates the default cape texture: textures/tex1_80x144_$_5.png

The mod draws the cape with a placeholder 80x144 RGB5A3 GX texture (format id 5). The file name
follows Dolphin's replacement convention (`tex1_<w>x<h>_<texhash>_<fmt>`); the `$` wildcard makes
it match that placeholder regardless of its content hash. 80x144 is deliberately unusual, so the
wildcard cannot collide with a texture the game itself uses.

To restyle the cape, just paint over textures/tex1_80x144_$_5.png (any resolution with the same
aspect ratio works; this one is 2x). Keep the file name. Alpha cuts the cape's silhouette.

Usage: python3 tools/make_default_texture.py
"""

import math
import random
from pathlib import Path

from PIL import Image

W, H = 160, 288  # 2x the 80x144 placeholder the game side sees


def lerp(a, b, t):
    return a + (b - a) * t


def in_triangle(px, py, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    d1 = sign((px, py), a, b)
    d2 = sign((px, py), b, c)
    d3 = sign((px, py), c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def in_triforce(x, y):
    """A small gold Triforce, low on the cape (it sits over the tunic flap when worn)."""
    cx, top_y, s = W / 2, 150, 17  # centre x, apex y, half-width of one triangle
    h = s * 1.7
    tris = [
        ((cx, top_y), (cx - s, top_y + h), (cx + s, top_y + h)),
        ((cx - s, top_y + h), (cx - 2 * s, top_y + 2 * h), (cx, top_y + 2 * h)),
        ((cx + s, top_y + h), (cx, top_y + 2 * h), (cx + 2 * s, top_y + 2 * h)),
    ]
    return any(in_triangle(x, y, *t) for t in tris)


def main():
    rng = random.Random(7)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = img.load()

    # base cloth: royal blue (matches the Hylian armor's tunic) fading a little darker toward the hem
    top = (34, 58, 128)
    bottom = (18, 32, 84)
    trim = (222, 184, 84)

    # vertical folds: a few sine bands of different frequency
    folds = [(3.0, 0.5), (5.5, 1.7), (8.0, 0.2)]

    # ragged hem
    hem = [0] * W
    for x in range(W):
        hem[x] = int(rng.uniform(0, 5) + 3 * math.sin(x * 0.35))

    for y in range(H):
        t = y / (H - 1)
        for x in range(W):
            if y > H - 1 - 4 - hem[x]:
                continue  # transparent: cuts the ragged hem
            u = x / (W - 1)
            shade = 0.0
            for freq, phase in folds:
                shade += math.sin(u * math.tau * freq + phase) * 0.035
            base = [lerp(top[i], bottom[i], t) * (1.0 + shade) for i in range(3)]

            # gold trim: 5 px on both side edges and the top, 7 px above the hem
            edge = min(x, W - 1 - x)
            hem_dist = (H - 1 - 4 - hem[x]) - y
            is_trim = edge < 5 or y < 6 or (hem_dist < 7 and hem_dist >= 0)
            # thin inner gold line, like the stitching on the tunic
            inner = (edge in (9, 10) and y > 9 and hem_dist > 10) or (y in (9, 10) and 9 <= x <= W - 10) \
                or (hem_dist in (10, 11) and 9 <= x <= W - 10)
            if is_trim or inner or in_triforce(x, y):
                k = 0.94 + 0.06 * math.sin((x + y) * 0.5)
                col = [trim[i] * k for i in range(3)]
            else:
                col = base
            px[x, y] = tuple(max(0, min(255, int(c))) for c in col) + (255,)

    out = Path(__file__).resolve().parent.parent / "textures" / "tex1_80x144_$_5.png"
    out.parent.mkdir(exist_ok=True)
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
