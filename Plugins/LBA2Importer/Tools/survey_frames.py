"""Survey of the frames the original puts on the walls it cut down for the camera (wooden caps, curved frames down
to the doorways, metal sills...), to seed the per-library frame lists of the enclosure settings, and of the bricks
without art (blank) per library.

A layout is a frame candidate when its instances sit at the top of their columns (nothing solid above) on a wall
(at least WALL solid cells right under them), in at least TOP_RATIO of its instances and MIN_COUNT of them. Lamps
and candles also sit on walls: the rendered sheets (-png) are checked by eye before seeding.
Usage: survey_frames.py [-png <dir>]"""
import collections, contextlib, io, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S
from hqr import entry

WALL = 4
TOP_RATIO = 0.7
MIN_COUNT = 6

def blank(brick, cache={}):
    if brick not in cache:
        d = entry(S.BKG, S.BRICK_START + brick - 1)
        cache[brick] = len(d) < 8 or d[1] == 0 or all(b == 0 for b in d[4:])
    return cache[brick]

args = sys.argv[1:]
png = args[args.index("-png") + 1] if "-png" in args else None
sys.argv = ["x"]
with contextlib.redirect_stdout(io.StringIO()):
    grids, zone_cells, found, cands = S.doors({}, None)
stats = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))  # lib -> layout -> [instances, on wall tops]
blanks = collections.defaultdict(collections.Counter)
for gi, g in grids.items():
    inst = collections.defaultdict(list)
    for (x, y, z) in zip(*np.nonzero(g.layout >= 0)):
        x, y, z = int(x), int(y), int(z)
        if g.brick[x, y, z] and blank(int(g.brick[x, y, z])):
            blanks[g.lib][int(g.layout[x, y, z])] += 1
            continue
        inst[g.anchor(x, y, z)].append((x, y, z))
    for (li, a), cells in inst.items():
        cols = collections.defaultdict(list)
        for (x, y, z) in cells:
            cols[(x, z)].append(y)
        top = True; wall = False
        for (x, z), ys in cols.items():
            hi, lo = max(ys), min(ys)
            if g.solid[x, hi + 1:, z].any() and any(g.brick[x, yy, z] and not blank(int(g.brick[x, yy, z])) for yy in range(hi + 1, 25)):
                top = False
            run = 0
            while lo - 1 - run >= 0 and g.solid[x, lo - 1 - run, z]:
                run += 1
            wall |= run >= WALL
        s = stats[g.lib][li]; s[0] += 1; s[1] += top and wall
picked = {}
for lib in sorted(stats):
    rows = [(li, n, k) for li, (n, k) in stats[lib].items() if k >= MIN_COUNT and k >= TOP_RATIO * n]
    rows.sort(key=lambda r: -r[2])
    picked[lib] = rows
    print(f"lib {lib:2d}: blank {blanks[lib].most_common(2)}  frames? " + " ".join(f"L{li}({k}/{n})" for li, n, k in rows))
if png:
    import subprocess
    for lib, rows in picked.items():
        if rows:
            subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "render_layouts.py"), str(lib),
                            ",".join(str(li) for li, _, _ in rows[:24]), os.path.join(png, f"frames_lib{lib:02d}.png")])
