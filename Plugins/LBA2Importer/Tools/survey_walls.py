"""Survey of the vertical brick sequences that make up walls, per library and per facing (indoor / outdoor).

A wall run is a vertical run of drawn bricks in one column whose camera-facing side (+X or +Z, the side the iso camera
saw and the art was drawn for) is open: the cell in front of each brick is empty. Its foot tells the facing:
- indoor: a floor in front at the run's foot (the far walls of a room, seen from inside),
- outdoor: nothing in front at its foot (the outer side of the camera-side walls, facades over lower ground).
Each run is a sequence of layout cells (layout, y in layout) from its foot up; runs are grouped per library by their
sequence, compressed to layout segments for printing (L33x3 = 3 cells of layout 33).

Usage: survey_walls.py [-lib N] [-min H] [-top K] [-v]
  -lib  only this library; -min  shortest run kept (default 3); -top  sequences printed per library and facing
  -v    also print the horizontal phases (x or z in the layout) seen per sequence
"""
import collections, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S
from hqr import entry


def blank(brick, cache={}):
    if brick not in cache:
        d = entry(S.BKG, S.BRICK_START + brick - 1)
        cache[brick] = len(d) < 8 or d[1] == 0 or all(b == 0 for b in d[4:])
    return cache[brick]

FACES = {"+X": (1, 0), "+Z": (0, 1)}


def runs(g):
    """Yields (facing, side, x, z, foot, cells) per wall run; cells = [(layout, ly, lh)] from the foot up."""
    for x in range(64):
        for z in range(64):
            for side, (dx, dz) in FACES.items():
                fx, fz = x + dx, z + dz
                inside = 0 <= fx < 64 and 0 <= fz < 64
                y = 0
                while y < 25:
                    def drawn(yy):
                        if g.layout[x, yy, z] < 0 or not g.brick[x, yy, z] or blank(int(g.brick[x, yy, z])):
                            return False
                        return not (inside and g.solid[fx, yy, fz])
                    if not drawn(y):
                        y += 1
                        continue
                    foot = y
                    while y < 25 and drawn(y):
                        y += 1
                    cells = []
                    for yy in range(foot, y):
                        li = int(g.layout[x, yy, z]); lo = g.layouts[li]; sx, sy, _ = lo["size"]; b = int(g.block[x, yy, z])
                        lx, ly, lz = b % sx, (b // sx) % sy, b // (sx * sy)
                        cells.append((li, ly, lz if side == "+X" else lx, (sx, sy, lo["size"][2])))
                    if inside and foot > 0 and g.solid[fx, foot - 1, fz]:
                        facing = "indoor"
                    elif not inside or not g.solid[fx, :foot, fz].any() or foot == 0:
                        facing = "outdoor"
                    else:
                        facing = "step"     # something in front but lower: a ledge, a stair, a lower floor
                    yield facing, side, x, z, foot, cells


def segments(cells):
    out = []
    for li, ly, *_ in cells:
        if out and out[-1][0] == li and ly != 0:
            out[-1][1] += 1
        else:
            out.append([li, 1])
    # Repeated layouts (a body stacked several times) as Lnn(h)*k.
    parts = []
    for li, n in out:
        if parts and parts[-1][0] == li and parts[-1][1] == n:
            parts[-1][2] += 1
        else:
            parts.append([li, n, 1])
    return " ".join((f"L{li}({n})" if n > 1 else f"L{li}") + (f"*{k}" if k > 1 else "") for li, n, k in parts)


def main():
    args = sys.argv[1:]
    only = int(args[args.index("-lib") + 1]) if "-lib" in args else None
    min_h = int(args[args.index("-min") + 1]) if "-min" in args else 3
    top = int(args[args.index("-top") + 1]) if "-top" in args else 12
    verbose = "-v" in args
    libs = {}
    stats = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))   # lib -> facing -> seq -> n
    phases = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    grids_of = collections.defaultdict(lambda: collections.defaultdict(set))
    heights = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for gi in range(S.GRM_START - S.GRID_START):
        g = S.Grid(gi, libs)
        if only is not None and g.lib != only:
            continue
        all_runs = [r for r in runs(g) if len(r[5]) >= min_h]
        feet = {"+X": set(), "+Z": set()}
        for facing, side, x, z, foot, cells in all_runs:
            feet[side].add((x, z, foot))
        for facing, side, x, z, foot, cells in all_runs:
            if len(cells) < min_h:
                continue
            # Wall panels only: every layout one cell deep toward the camera (not furniture), and the run part of a wall
            # line (the columns on both sides along the wall have a run with the same foot).
            depth = 0 if side == "+X" else 2
            if any(size[depth] != 1 for *_, size in cells):
                continue
            ax, az = (0, 1) if side == "+X" else (1, 0)
            if sum((x + k * ax, z + k * az, foot) in feet[side] for k in (-1, 1)) < 1:
                continue
            key = tuple((li, ly) for li, ly, *_ in cells)
            stats[g.lib][facing][key] += 1
            heights[g.lib][facing][len(cells)] += 1
            phases[g.lib][key][tuple(lh for _, _, lh, _ in cells)] += 1
            grids_of[g.lib][key].add(gi)
    for lib in sorted(stats):
        print(f"\n=== lib {lib}")
        for facing in ("indoor", "outdoor", "step"):
            c = stats[lib][facing]
            if not c:
                continue
            hs = heights[lib][facing]
            print(f"  {facing}: {sum(c.values())} runs, {len(c)} sequences, heights {sorted(hs.items())}")
            for key, n in c.most_common(top):
                gs = sorted(grids_of[lib][key])
                line = f"    {n:4d}x h{len(key):2d} {segments([(li, ly, 0) for li, ly in key])}   grids {gs[:6]}{'...' if len(gs) > 6 else ''}"
                if verbose:
                    line += f"   phases {phases[lib][key].most_common(3)}"
                print(line)


if __name__ == "__main__":
    main()
