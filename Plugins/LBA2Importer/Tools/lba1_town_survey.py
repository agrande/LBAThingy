"""Surveys of LBA1's town scenes for the kit, walls and roofs work (LBA1_ISLANDS_PLAN.md, Phase 3). Writes to Saved/LookDev/lba1_kit.

Usage: lba1_town_survey.py <command> [args]
  iso <scene> [...]   the scene's grid drawn as the iso camera did (iso_<scene>.png): to see how a house is built
  bricks              the 160 most used bricks with cell counts, stacked counts and collision shapes (bricks.png)
  walls               wall bricks (both sides opaque, no top: LBAPlanetDistricts' IsWallBrick) by layout size and style
  ramps               the most used bricks of each ramp shape 2-5 (ramps.png): 4 and 5 hold a slope's art, 2 and 3 slivers
  section <scene>     a vertical slice through the scene's thatch cells, printing shape per cell (T: thatch)
  left                pieces neither ground, walls nor in the kit, by cells (count x size): what is still drawn as bricks

Cells are (x, y up, z), 64 cm across, 32 cm up. Collision shapes: 1 full, 2-5 ramps rising toward +x, +z, -z, -x, 6-13 corners.
"""
import collections, json, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lba1

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = os.path.join(PROJECT, "Saved/LookDev/lba1_kit")
SCENES = [11, 12, 13, 17, 18, 19, 25, 24, 37, 42, 44, 45, 46, 47, 59]

YY, XX = np.mgrid[0:38, 0:48] + 0.5
EDGE = 24 - np.abs(XX - 24) * 0.5
TOP = YY <= EDGE
LEFT = (YY > EDGE) & (YY < EDGE + 15) & (XX < 24)
RIGHT = (YY > EDGE) & (YY < EDGE + 15) & (XX >= 24)


def cells(n):
    """Per cell: brick, shape, layout (0-based) and block, as arrays [x, y, z]."""
    grid = lba1.load_grid(n); lib = lba1.load_library(n)
    shape = [np.zeros(grid.shape, np.int32) for _ in range(4)]
    for x, y, z in zip(*np.nonzero(grid)):
        v = grid[x, y, z]; layout, block = (v >> 8) - 1, v & 255
        if 0 <= layout < len(lib) and block < len(lib[layout]["blocks"]):
            sh, ty, br = lib[layout]["blocks"][block]
            if br and ty != lba1.WATER_BRICK:
                shape[0][x, y, z] = br; shape[1][x, y, z] = sh; shape[2][x, y, z] = layout; shape[3][x, y, z] = block
    return shape


def is_wall(brick):
    a = lba1.brick(brick)[..., 3] > 0
    return a[LEFT].mean() >= 0.9 and a[RIGHT].mean() >= 0.9 and a[TOP].mean() <= 0.1


def thatch(brick):
    a = lba1.brick(brick); m = a[..., 3] > 0
    if m.sum() < 30:
        return False
    c = a[m][:, :3].astype(float)
    return ((c[:, 0] > 150) & (c[:, 1] > 110) & (c[:, 2] < 90)).mean() > 0.15


def sheet(items, path, columns=16, cell=(96, 96), scale=(72, 57)):
    out = Image.new("RGB", (columns * cell[0], ((len(items) + columns - 1) // columns) * cell[1]), (40, 40, 48))
    draw = ImageDraw.Draw(out)
    for i, (brick, label) in enumerate(items):
        sprite = Image.fromarray(lba1.brick(brick), "RGBA").resize(scale, Image.NEAREST)
        at = (i % columns * cell[0], i // columns * cell[1])
        out.paste(sprite, (at[0] + 12, at[1] + 14), sprite)
        draw.text((at[0] + 2, at[1] + 1), label, fill=(255, 255, 255))
    out.save(path); print(path)


def iso(scenes):
    for n in scenes:
        bricks = cells(int(n))[0]
        canvas = np.zeros((128 * 12 + 25 * 15 + 40, 128 * 24 + 48, 3), np.uint8) + 40
        for x, y, z in sorted(zip(*np.nonzero(bricks)), key=lambda p: (p[0] + p[2], p[1], p[0])):
            img = lba1.brick(int(bricks[x, y, z])); m = img[..., 3] > 0
            px, py = (63 - z + x) * 24, (x + z) * 12 + (24 - y) * 15
            canvas[py:py + 38, px:px + 48][m] = img[..., :3][m]
        path = os.path.join(OUT, "iso_%s.png" % n); Image.fromarray(canvas).save(path); print(path)


def bricks_cmd():
    count = collections.Counter(); stacked = collections.Counter(); shapes = collections.defaultdict(set)
    for n in SCENES:
        b, s, _, _ = cells(n)
        for x, y, z in zip(*np.nonzero(b)):
            count[b[x, y, z]] += 1; shapes[b[x, y, z]].add(int(s[x, y, z]))
            if 0 < y < 24 and b[x, y + 1, z] and b[x, y - 1, z]:
                stacked[b[x, y, z]] += 1
    sheet([(b, "%d x%d s%d sh%s" % (b, c, stacked[b], ",".join(map(str, sorted(shapes[b]))))) for b, c in count.most_common(160)],
          os.path.join(OUT, "bricks.png"))


def walls():
    by_size = collections.Counter(); styles = collections.Counter(); total = 0
    for n in SCENES:
        lib = lba1.load_library(n); b, s, layout, block = cells(n)
        for x, y, z in zip(*np.nonzero(b)):
            if not is_wall(int(b[x, y, z])):
                continue
            total += 1; lay = lib[layout[x, y, z]]; sx, sy, sz = lay["size"]
            by_size["%dx%dx%d" % (sx, sy, sz)] += 1
            if sx == 1 and sz == 1:
                styles[tuple(q[2] for q in lay["blocks"])] += 1
    print("wall-brick cells", total, "in column layouts (styles)", sum(styles.values()))
    print("by layout size", by_size.most_common(15))
    print("styles", styles.most_common(15))


def ramps():
    by = collections.defaultdict(collections.Counter)
    for n in SCENES:
        b, s, _, _ = cells(n)
        for x, y, z in zip(*np.nonzero(b)):
            if 2 <= s[x, y, z] <= 13:
                by[int(s[x, y, z])][int(b[x, y, z])] += 1
    print({k: sum(v.values()) for k, v in sorted(by.items())})
    items = []
    for shape in (2, 3, 4, 5):
        items += [(br, "s%d b%d x%d" % (shape, br, c)) for br, c in by[shape].most_common(12)]
    sheet(items, os.path.join(OUT, "ramps.png"), columns=12, cell=(100, 110), scale=(96, 76))


def section(n):
    b, s, _, _ = cells(int(n))
    t = np.vectorize(lambda q: bool(q) and thatch(int(q)))(b)
    x = int(np.argmax(t.sum(axis=(1, 2)))); zs = np.nonzero(t[x].any(0))[0]
    print("scene %s, slice x=%d, z from %d" % (n, x, zs.min()))
    for y in range(24, -1, -1):
        row = "".join(("%s%d" % ("T" if t[x, y, z] else "#" if s[x, y, z] == 1 else "." if not b[x, y, z] else "s", s[x, y, z])).ljust(4)
                      for z in range(max(0, zs.min() - 1), min(64, zs.min() + 16)))
        if row.strip(". 0"):
            print("  y%2d %s" % (y, row))


def left():
    pieces = json.load(open(os.path.join(OUT, "pieces.json")))
    kit = json.load(open(os.path.join(PROJECT, "Plugins/LBA2Importer/Resources/Models/lba1_kit.json")))
    examples = {p["example"] for p in kit["pieces"]}
    for p in sorted(pieces, key=lambda p: -p["count"] * np.prod(p["size"]))[:60]:
        size = p["size"]
        column_wall = size[0] == 1 and size[2] == 1 and any(is_wall(b) for b in p["bricks"])
        if (size[1] == 1 and p["count"] > 200) or p["example"] in examples or column_wall:
            continue
        print("%s x%d %s cells %d bricks %s" % (p["example"], p["count"], "x".join(map(str, size)), p["count"] * np.prod(size), p["bricks"][:4]))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    command, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    {"iso": lambda: iso(args), "bricks": bricks_cmd, "walls": walls, "ramps": ramps, "section": lambda: section(args[0]),
     "left": left}.get(command, lambda: print(__doc__))()
