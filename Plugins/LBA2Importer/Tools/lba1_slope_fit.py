"""Prototype: does a ramp plane explain an LBA1 layout's art? For Proxim City's roofs (LBA1_ISLANDS_PLAN.md, "Proxim City's roofs").

Usage: lba1_slope_fit.py <scene>:<layout> [...]     e.g. 42:186 42:187 42:188 42:189 42:190 13:204
       lba1_slope_fit.py all                         every solid layout of the towns' scenes that fits (false positives too)

The iso camera drew a cell's point (x, y up, z; 0..1) at sprite pixel (24 + 24x - 24z, 12x + 12z + 15(1 - y)); a layout's cells
compose at (sz-1-z+x)*24, (x+z)*12 + (sy-1-y)*15 (as lba1_kit_survey.draw_piece). A ramp plane climbs one step (32 cm) per cell
(64 cm), as the collision ramps 2-5. For each shape and offset, the plane's cells are drawn as that ramp and the layout's other
cube-like bricks (art over cube at least 0.5) as cubes; the score is intersection over union with the layout's art.

Results (2026-11-07): 42:186 and 42:188 (facing, shape 5) 0.89; 42:190 (edge on, shape 2) 0.78; the composites 42:187 0.62 and
the gable end 42:189 0.34; false positives at 0.71-0.78 among palisades (24:59, 24:60, 59:59, 59:60) and a Principal 1x8x3 piece.
Drawing the fitted cells with their own sprites left holes: a piece's slope art straddles the cells' sprites. Read the piece's
composed image instead, if this is tried again.
"""
import collections, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lba1

YY, XX = np.mgrid[0:38, 0:48] + 0.5


def iso(x, y, z):
    return (24 + 24 * x - 24 * z, 12 * x + 12 * z + 15 * (1 - y))


def ramp_height(shape, x, z):
    return {2: x, 3: z, 4: 1 - z, 5: 1 - x}[shape]


def climb(shape, x, z):
    return {2: x, 3: z, 4: -z, 5: -x}[shape]


def polygon(points):
    mask = np.ones(XX.shape, bool); n = len(points)
    area = sum(points[i][0] * points[(i + 1) % n][1] - points[(i + 1) % n][0] * points[i][1] for i in range(n))
    for i in range(n):
        (x0, y0), (x1, y1) = points[i], points[(i + 1) % n]
        side = (x1 - x0) * (YY - y0) - (y1 - y0) * (XX - x0)
        mask &= (side >= 0) if area > 0 else (side <= 0)
    return mask


CUBE = (polygon([iso(0, 1, 0), iso(1, 1, 0), iso(1, 1, 1), iso(0, 1, 1)]) | polygon([iso(1, 1, 0), iso(1, 0, 0), iso(1, 0, 1), iso(1, 1, 1)])
        | polygon([iso(0, 1, 1), iso(1, 1, 1), iso(1, 0, 1), iso(0, 0, 1)]))
RAMP = {s: polygon([iso(x, ramp_height(s, x, z), z) for x, z in ((0, 0), (1, 0), (1, 1), (0, 1))]) for s in (2, 3, 4, 5)}


def fit(layout, min_climb=3):
    """Best (score, shape, offset, plane cells, kept cubes) or None; cells are (x, y, z) in the layout."""
    sx, sy, sz = layout["size"]
    width, height = (sx + sz) * 24 + 48, (sx + sz) * 12 + sy * 15 + 38
    cells = {}
    for i, (shape, _, brick) in enumerate(layout["blocks"]):
        if brick and shape != 1:
            return None  # real ramps are drawn as slopes already
        if brick and brick != 223:
            cells[(i % sx, (i // sx) % sy, i // (sx * sy))] = lba1.brick(brick)[..., 3] > 0
    if len(cells) < 2:
        return None

    def place(mask, cell, canvas):
        x, y, z = cell
        px, py = (sz - 1 - z + x) * 24, (x + z) * 12 + (sy - 1 - y) * 15
        canvas[py:py + 38, px:px + 48] |= mask

    art = np.zeros((height, width), bool)
    for cell, mask in cells.items():
        place(mask, cell, art)
    cubelike = {c: (m & CUBE).sum() >= 0.5 * (m | CUBE).sum() for c, m in cells.items()}
    best = None
    for shape in (2, 3, 4, 5):
        for k in range(-(sx + sy + sz), sx + sy + sz + 1):
            plane = [(x, climb(shape, x, z) + k, z) for x in range(sx) for z in range(sz) if 0 <= climb(shape, x, z) + k < sy]
            if len({c[1] for c in plane}) < min_climb:
                continue
            drawn = np.zeros((height, width), bool)
            kept = [c for c in cells if cubelike[c] and c not in plane]
            for c in plane:
                place(RAMP[shape], c, drawn)
            for c in kept:
                place(CUBE, c, drawn)
            score = (art & drawn).sum() / max(1, (art | drawn).sum())
            if not best or score > best[0]:
                best = (score, shape, k, plane, kept)
    return best


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit()
    if sys.argv[1] == "all":
        seen = {}
        for n in [11, 12, 13, 17, 18, 19, 25, 24, 37, 42, 44, 45, 46, 47, 59]:
            lib = lba1.load_library(n); grid = lba1.load_grid(n); used = collections.Counter()
            for v in grid[np.nonzero(grid)]:
                if v & 255 == 0 and 1 <= v >> 8 <= len(lib):
                    used[(v >> 8) - 1] += 1
            for li, count in used.items():
                key = (tuple(lib[li]["size"]), tuple(b[2] for b in lib[li]["blocks"]))
                if key not in seen:
                    seen[key] = ("%d:%d" % (n, li), fit(lib[li]), count)
        for example, result, count in sorted(seen.values(), key=lambda v: -v[2]):
            if result and result[0] >= 0.7:
                print("%s x%d score %.2f shape %d offset %d plane %s" % (example, count, *result[:4]))
    else:
        for arg in sys.argv[1:]:
            n, li = map(int, arg.split(":"))
            result = fit(lba1.load_library(n)[li])
            print(arg, "no fit" if not result else "score %.2f shape %d offset %d plane %s cubes %s" % result)
