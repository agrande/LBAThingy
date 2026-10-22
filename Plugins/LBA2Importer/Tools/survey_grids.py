"""Surveys LBA2 iso interiors (LBA_BKG.HQR) for the enclosure design: rooms, wall edges, ceilings, doorway layouts.

Usage: survey_grids.py [grid ...] [-v] [-png <dir>]
  grids    grid indices to report (default all); door statistics always use every interior scene
  -v       per-grid edge details
  -png     render doorway candidate layouts per library into <dir>

Axes are the grid's: X, Z horizontal, Y up (cells 512x256x512 units). The iso camera looks from +X +Z, so a room's
+X and +Z sides face the camera ("near"), -X and -Z are the back ("far").

Reports: walls and ceilings per grid and library; cube-change zones and the layouts found next to them (doorway
candidates, rendered with -png); doorways found by DoorFinder (see its docstring), checked against the zones. With
-png, listed grids also get a top-view map (map_gridNNN.png) with zones and doorways.
"""
import collections, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hqr import entry
from dump_scenes import parse as parse_scene, GAME

BKG = GAME + "LBA_BKG.HQR"
HEADROOM = 4          # empty cells above a walkable floor (Twinsen + margin)
WALL_MIN = 3          # solid cells above the floor for an edge to count as a wall
DIRS = {"+X": (1, 0), "-X": (-1, 0), "+Z": (0, 1), "-Z": (0, -1)}

hdr = entry(BKG, 0)
GRID_START, GRM_START, LIB_START, BRICK_START, NUM_BRICKS, FORBIDDEN = struct.unpack_from("<6H", hdr, 0)


def load_library(index):
    d = entry(BKG, LIB_START + index)
    layouts = []
    for li in range(struct.unpack_from("<I", d, 0)[0] // 4):
        at = struct.unpack_from("<I", d, li * 4)[0]
        sx, sy, sz = (max(1, v) for v in d[at:at + 3])
        blocks = [struct.unpack_from("<BBH", d, at + 3 + b * 4) for b in range(sx * sy * sz)]
        layouts.append(dict(size=(sx, sy, sz), blocks=blocks))  # block = (shape, code, brick), index z*sx*sy + y*sx + x
    return layouts


def load_grid(index):
    d = entry(BKG, GRID_START + index)
    layout = np.full((64, 25, 64), -1, np.int16)
    block = np.zeros((64, 25, 64), np.uint8)
    for col in range(64 * 64):
        x, z = col % 64, col // 64
        at = struct.unpack_from("<H", d, 34 + col * 2)[0] + 34
        runs = d[at]; at += 1; y = 0
        for _ in range(runs):
            if y >= 25:
                break
            flags = d[at]; at += 1
            typ, h = flags >> 6, (flags & 31) + 1
            if typ == 2:
                layout[x, y:y + h, z] = d[at] - 1; block[x, y:y + h, z] = d[at + 1]; at += 2
            elif typ == 1:
                for k in range(h):
                    if y + k < 25:
                        layout[x, y + k, z] = d[at] - 1; block[x, y + k, z] = d[at + 1]
                    at += 2
            y += h
    return d[0], layout, block


def scene_map():
    d = entry(BKG, BRICK_START + NUM_BRICKS)
    out = []
    for at in range(0, len(d) - 1, 2):
        if d[at] == 0:
            break
        out.append((d[at] == 2, d[at + 1]))
    return out


class Grid:
    def __init__(self, index, libs):
        self.index = index
        self.lib, self.layout, self.block = load_grid(index)
        L = libs.setdefault(self.lib, load_library(self.lib))
        self.layouts = L
        self.shape = np.zeros((64, 25, 64), np.uint8)
        self.brick = np.zeros((64, 25, 64), np.uint16)
        for (x, y, z) in zip(*np.nonzero(self.layout >= 0)):
            lo = L[self.layout[x, y, z]] if self.layout[x, y, z] < len(L) else None
            b = self.block[x, y, z]
            if lo and b < len(lo["blocks"]):
                s, _, br = lo["blocks"][b]
                self.shape[x, y, z] = s
                self.brick[x, y, z] = br if br != FORBIDDEN else 0
        empty = self.layout < 0
        self.shape[empty] = self.block[empty]   # invisible collision cells
        self.solid = self.shape != 0

    def solid_at(self, x, y, z):
        return 0 <= x < 64 and 0 <= z < 64 and 0 <= y < 25 and self.solid[x, y, z]

    def anchor(self, x, y, z):
        """Origin cell of the layout instance covering (x, y, z)."""
        li = int(self.layout[x, y, z]); lo = self.layouts[li]
        sx, sy, _ = lo["size"]; b = int(self.block[x, y, z])
        return li, (x - b % sx, y - (b // sx) % sy, z - b // (sx * sy))

    def floors(self):
        f = set()
        for (x, y, z) in zip(*np.nonzero(self.solid)):
            if y + 1 < 25 and not self.solid[x, y + 1:y + 1 + HEADROOM, z].any():
                f.add((int(x), int(y), int(z)))
        return f

    def spaces(self):
        floors = self.floors(); seen = set(); out = []
        for start in sorted(floors):
            if start in seen:
                continue
            comp = []; todo = [start]; seen.add(start)
            while todo:
                x, y, z = todo.pop(); comp.append((x, y, z))
                for dx, dz in DIRS.values():
                    for dy in (0, 1, -1):
                        n = (x + dx, y + dy, z + dz)
                        if n in floors and n not in seen:
                            seen.add(n); todo.append(n)
            if len(comp) >= 16:
                out.append(comp)
        return out

    def classify(self, x, y, z, d):
        """Kind of the boundary between floor cell (x,y,z) and its neighbor in direction d."""
        dx, dz = DIRS[d]; nx, nz = x + dx, z + dz
        if not (0 <= nx < 64 and 0 <= nz < 64):
            return "border", 0, False
        h = 0
        while y + 1 + h < 25 and self.solid[nx, y + 1 + h, nz]:
            h += 1
        if h >= WALL_MIN:
            visible = bool(self.brick[nx, y + 1:y + 1 + h, nz].any())
            return ("wall" if visible else "invis"), h, y + 1 + h >= 25
        if h > 0:
            return "low", h, False
        below = [yy for yy in range(y, -1, -1) if self.solid[nx, yy, nz]]
        if below and below[0] == y:
            return "lowroof", 0, False      # same floor but not enough headroom (under furniture, low ceiling)
        return ("drop" if below else "void"), (y - below[0]) if below else 0, False


def survey(grids, libs, verbose):
    lib_walls = collections.defaultdict(lambda: collections.defaultdict(list))   # lib -> near/far -> heights
    lib_ceil = collections.defaultdict(list)
    print(f"{'grid':>4} {'lib':>3} {'spaces':>6} {'cells':>5}  per side: wall/invis/low/drop/void (+X +Z = near, -X -Z = far)   ceiling")
    for gi in grids:
        g = Grid(gi, libs)
        sp = g.spaces()
        counts = {d: collections.Counter() for d in DIRS}
        heights = {d: [] for d in DIRS}
        cells = 0; ceil = []
        for comp in sp:
            members = set(comp); cols = {(x, z) for x, _, z in comp}
            cells += len(comp)
            for (x, y, z) in comp:
                for d, (dx, dz) in DIRS.items():
                    if (x + dx, z + dz) in cols and any((x + dx, y + k, z + dz) in members for k in (-1, 0, 1)):
                        continue
                    kind, h, clipped = g.classify(x, y, z, d)
                    counts[d][kind] += 1
                    if kind in ("wall", "invis") and not clipped:
                        heights[d].append(h)
                        lib_walls[g.lib]["near" if d[0] == "+" else "far"].append(h)
                above = np.nonzero(g.solid[x, y + 1:, z])[0]
                ceil.append(int(above[0]) + 1 if len(above) else None)
        covered = [c for c in ceil if c is not None]
        lib_ceil[g.lib] += covered
        side = "  ".join(f"{d}:{c['wall']}/{c['invis']}/{c['low']}/{c['drop']}/{c['void']}" for d, c in counts.items())
        cov = f"{100 * len(covered) // max(1, len(ceil))}% med {int(np.median(covered)) if covered else '-'}"
        print(f"{gi:4d} {g.lib:3d} {len(sp):6d} {cells:5d}  {side}   {cov}")
        if verbose:
            for d in DIRS:
                hs = collections.Counter(heights[d])
                print(f"        {d} wall heights {sorted(hs.items())}  other {dict(counts[d])}")
    print("\nPer library: wall height above floor (cells), near vs far sides; ceiling height where covered")
    for lib in sorted(lib_walls):
        far = lib_walls[lib]["far"]; near = lib_walls[lib]["near"]; c = lib_ceil[lib]
        mode = lambda v: collections.Counter(v).most_common(3)
        print(f"  lib {lib:2d}: far n={len(far):5d} med={np.median(far) if far else '-':>4} top={mode(far)}"
              f" | near n={len(near):5d} med={np.median(near) if near else '-':>4} top={mode(near)}"
              f" | ceiling n={len(c)} top={mode(c)}")


def doors(libs, png_dir):
    smap = scene_map()
    grids = {}
    near_door = collections.defaultdict(lambda: collections.defaultdict(set))   # lib -> layout -> anchors near doors
    zone_hits = collections.defaultdict(collections.Counter)                     # lib -> layout -> zones
    placement = collections.Counter()
    nzones = collections.Counter()
    zone_boxes = collections.defaultdict(list)                                   # grid -> [(cell box, walk-in)]
    zone_cells = collections.defaultdict(list)                                   # grid -> [((x0, x1), (y0, y1), (z0, z1))]
    for scene in range(min(len(smap), 222)):
        is_island, gi = smap[scene]
        if is_island:
            continue
        try:
            s = parse_scene(scene)
        except Exception:
            continue
        if gi not in grids:
            grids[gi] = Grid(gi, libs)
        g = grids[gi]
        for z in s["zones"]:
            if z["type"] != 0:
                continue
            x0, y0, z0, x1, y1, z1 = z["box"]
            cx0, cx1 = sorted((x0 // 512, x1 // 512)); cz0, cz1 = sorted((z0 // 512, z1 // 512))
            cy0, cy1 = sorted((y0 // 256, y1 // 256))
            box = (slice(max(0, cx0 - 1), min(64, cx1 + 2)), slice(max(0, cy0 - 1), min(25, cy1 + 3)), slice(max(0, cz0 - 1), min(64, cz1 + 2)))
            nzones[g.lib] += 1
            zone_boxes[gi].append((box, z["info"][5] & 1))
            zone_cells[gi].append(((cx0, cx1), (cy0, cy1), (cz0, cz1)))
            seen = set()
            lay = g.layout[box]
            for (x, y, zz) in zip(*np.nonzero(lay >= 0)):
                X, Y, Z = x + box[0].start, y + box[1].start, zz + box[2].start
                li, a = g.anchor(X, Y, Z)
                near_door[g.lib][li].add((gi, a))
                seen.add(li)
            for li in seen:
                zone_hits[g.lib][li] += 1
            # Where the door sits: walk-in door painted on a wall, a gap in a wall (solid around it above the
            # floor), or open ground. Floor level = lowest walkable cell under the zone.
            floor_y = cy0
            above = g.solid[box][:, max(0, floor_y + 1 - box[1].start):, :]
            placement["walk-in (Info5&1)" if z["info"][5] & 1 else ("gap in wall" if above.sum() >= 4 else "open")] += 1

    # Every instance of every layout over all grids, to tell door-specific layouts from common ones.
    totals = collections.defaultdict(collections.Counter)
    for gi in range(GRM_START - GRID_START):
        g = grids.get(gi) or Grid(gi, libs)
        grids[gi] = g
        anchors = set()
        for (x, y, z) in zip(*np.nonzero(g.layout >= 0)):
            anchors.add(g.anchor(x, y, z))
        for li, _ in anchors:
            totals[g.lib][li] += 1

    print("\nCube-change zones in interiors by placement:", dict(placement))
    print("Doorway layout candidates (zones >= 2, at least half of their instances next to a cube-change zone):")
    cands = {}
    for lib in sorted(zone_hits):
        rows = []
        for li, n in zone_hits[lib].items():
            near = len(near_door[lib][li]); tot = totals[lib][li]
            if n >= 2 and near * 2 >= tot:
                lo = libs[lib][li]
                rows.append((n, li, near, tot, lo["size"], arch(lo)))
        rows.sort(reverse=True)
        cands[lib] = rows
        print(f"  lib {lib:2d} ({nzones[lib]} zones): " + ", ".join(
            f"L{li}x{n} [{near}/{tot} {sz[0]}x{sz[1]}x{sz[2]}{' arch' if a else ''}]" for n, li, near, tot, sz, a in rows[:12]))
    if png_dir:
        render(libs, cands, png_dir)
    door_cells, found = doorways(grids, cands, zone_boxes, zone_cells)
    if png_dir:
        for gi in MAP_GRIDS:
            draw_map(grids[gi], zone_cells.get(gi, []), door_cells.get(gi, {}), png_dir)
    return grids, zone_cells, found, cands


MAP_GRIDS = []


def draw_map(g, boxes, doors, out):
    """Top view (X right, Z down): height of the top solid cell, walls >= 3 cells above their floor darker,
    cube zones red; doorways green (between rooms), blue (exits), yellow (in a camera-side stub)."""
    from PIL import Image, ImageDraw
    S = 10
    img = Image.new("RGB", (64 * S, 64 * S), (16, 16, 20)); dr = ImageDraw.Draw(img)
    for x in range(64):
        for z in range(64):
            ys = np.nonzero(g.solid[x, :, z])[0]
            if not len(ys):
                continue
            top = int(ys[-1]); run = 0
            while top - run >= 0 and g.solid[x, top - run, z]:
                run += 1
            v = 60 + top * 7
            col = (v // 2, v // 2, v) if run >= 3 else (v, v, v // 2 + 40)
            dr.rectangle([x * S, z * S, x * S + S - 1, z * S + S - 1], fill=col)
    colors = {"interior": (40, 220, 60), "exit": (40, 200, 255), "stub": (255, 210, 40)}
    for (x, y, z), kind in doors.items():
        dr.rectangle([x * S + 2, z * S + 2, x * S + S - 3, z * S + S - 3], fill=colors[kind])
    for (x0, x1), _, (z0, z1) in boxes:
        dr.rectangle([x0 * S, z0 * S, (x1 + 1) * S - 1, (z1 + 1) * S - 1], outline=(255, 40, 40), width=2)
    dr.text((4, 4), f"grid {g.index} lib {g.lib}", fill=(255, 255, 0))
    img.save(os.path.join(out, f"map_grid{g.index:03d}.png"))


WALL_H = 3     # wall cells above the floor for a doorway flank (the camera-side walls are cut down to 3-4)
TALL_H = 5     # flanks lower than this are camera-side stubs
MAX_WIDTH = 5  # doorway width in cells
MAX_DEPTH = 3       # cells through the wall between two rooms; deeper openings are corridors
MAX_EXIT_DEPTH = 8  # exits often go through a short passage (to the grid edge or the plug behind the zone)


class DoorFinder:
    """Doorways: openings in a wall line, found on the cell grid.

    A doorway cell is a walkable floor cell with wall columns (>= WALL_H solid cells above its floor) on both sides
    along one axis, at most MAX_WIDTH apart, that are part of a wall line (a flank has another wall column next to
    it, not a lone pillar). Along the other axis the opening goes through the wall's thickness, and its two ends are
    the room on one side and the room (interior doorway, at most MAX_DEPTH deep) or an invisible collision plug (how
    the original closes an exit behind its cube-change zone), a void or the grid edge (exit, at most MAX_EXIT_DEPTH
    deep) on the other.

    An opening is kept as a doorway when it has a lintel (solid cells above it), one of its flanks is a jamb layout
    (jambs, arches, posts: camera-side doorways lost their lintel when the wall was cut down), its floor is a
    threshold layout (doormats) or a cube-change zone is at it; and it does not only end at lower floors (balconies,
    steps down) and is at most MAX_CELLS cells.
    """

    MAX_CELLS = 15

    def __init__(self, g, jambs=(), thresholds=(), zones=()):
        self.g = g
        # Only floors of walkable spaces: small pockets behind walls are decor the camera never reaches.
        self.floors = {c for space in g.spaces() for c in space}
        self.jambs = set(jambs)
        self.thresholds = set(thresholds)
        self.zones = list(zones)

    def wall(self, x, y, z, h=WALL_H):
        g = self.g
        return 0 <= x < 64 and 0 <= z < 64 and y + h < 25 and bool(g.solid[x, y + 1:y + 1 + h, z].all())

    def wall_height(self, x, y, z):
        h = 0
        while y + 1 + h < 25 and self.g.solid[x, y + 1 + h, z]:
            h += 1
        return h

    def end_state(self, x, y, z):
        """What lies at the end of an opening: room, plug (invisible collision), void, edge, drop or blocked."""
        g = self.g
        if not (0 <= x < 64 and 0 <= z < 64):
            return "edge"
        if any((x, y + k, z) in self.floors for k in (-1, 0, 1)):
            return "room"
        top = min(25, y + 1 + WALL_H)
        if g.solid[x, y + 1:top, z].any():
            return "blocked" if g.brick[x, y + 1:top, z].any() else "plug"
        return "drop" if g.solid[x, :y + 1, z].any() else "void"

    def lined(self, x, y, z, away):
        """A flank is part of a wall line: another wall column next to it (not back into the opening)."""
        for dx, dz in DIRS.values():
            if (dx, dz) != away and self.wall(x + dx, y, z + dz):
                return True
        return x in (0, 63) or z in (0, 63)

    def cells(self):
        """Doorway cells: {(x, y, z): info}."""
        hits = {}
        for (x, y, z) in self.floors:
            for ax, sd in (((1, 0), (0, 1)), ((0, 1), (1, 0))):
                if self.wall(x, y, z):
                    continue
                left = next((k for k in range(1, MAX_WIDTH + 1) if self.wall(x - k * sd[0], y, z - k * sd[1])), None)
                right = next((k for k in range(1, MAX_WIDTH + 1) if self.wall(x + k * sd[0], y, z + k * sd[1])), None)
                if left is None or right is None or left + right - 1 > MAX_WIDTH:
                    continue
                lx, lz = x - left * sd[0], z - left * sd[1]
                rx, rz = x + right * sd[0], z + right * sd[1]
                if not (self.lined(lx, y, lz, sd) and self.lined(rx, y, rz, (-sd[0], -sd[1]))):
                    continue

                # Depth through the wall: how far along the passage both flanks continue.
                def flanked(k):
                    return self.wall(lx + k * ax[0], y, lz + k * ax[1]) and self.wall(rx + k * ax[0], y, rz + k * ax[1])
                neg = next((k for k in range(1, MAX_EXIT_DEPTH + 2) if not flanked(-k)), MAX_EXIT_DEPTH + 1) - 1
                pos = next((k for k in range(1, MAX_EXIT_DEPTH + 2) if not flanked(k)), MAX_EXIT_DEPTH + 1) - 1
                depth = neg + pos + 1
                if depth > MAX_EXIT_DEPTH:
                    continue
                ends = sorted((self.end_state(x - (neg + 1) * ax[0], y, z - (neg + 1) * ax[1]),
                               self.end_state(x + (pos + 1) * ax[0], y, z + (pos + 1) * ax[1])))
                if "room" not in ends or "blocked" in ends:
                    continue
                other = ends[0] if ends[1] == "room" else ends[1]
                if other == "room" and depth > MAX_DEPTH:
                    continue
                above = np.nonzero(self.g.solid[x, y + 1:, z])[0]
                hits[(x, y, z)] = dict(
                    axis="X" if ax == (1, 0) else "Z",
                    width=left + right - 1,
                    flank=min(self.wall_height(lx, y, lz), self.wall_height(rx, y, rz)),
                    clear=int(above[0]) if len(above) else None,
                    kind="interior" if other == "room" else "exit",
                    end=other,
                    layouts={self.g.anchor(fx, yy, fz)[0] for fx, fz in ((lx, lz), (rx, rz))
                             for yy in range(y + 1, min(25, y + 1 + TALL_H)) if self.g.layout[fx, yy, fz] >= 0},
                    floor=self.g.anchor(x, y, z)[0] if self.g.layout[x, y, z] >= 0 else None)
        return hits

    def doorways(self):
        """Groups doorway cells into doorways: [dict(cells, kind, end, width, flank, clear, layouts, signals)]."""
        hits = self.cells(); seen = set(); out = []
        for c in sorted(hits):
            if c in seen:
                continue
            # One doorway = neighboring cells crossed the same way (axis) to the same kind of place.
            key = (hits[c]["axis"], hits[c]["kind"])
            todo = [c]; seen.add(c); cells = []
            while todo:
                p = todo.pop(); cells.append(p)
                for dx in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            n = (p[0] + dx, p[1] + dy, p[2] + dz)
                            if n in hits and n not in seen and (hits[n]["axis"], hits[n]["kind"]) == key:
                                seen.add(n); todo.append(n)
            infos = [hits[p] for p in cells]
            clears = [i["clear"] for i in infos if i["clear"] is not None]
            layouts = set().union(*(i["layouts"] for i in infos))
            floors = {i["floor"] for i in infos}
            ends = {i["end"] for i in infos}
            signals = [name for name, on in (("lintel", bool(clears)), ("jamb", bool(layouts & self.jambs)),
                                             ("threshold", bool(floors & self.thresholds)),
                                             ("zone", any(near(cells, box) for box in self.zones))) if on]
            if not signals or ends == {"drop"} or len(cells) > self.MAX_CELLS:
                continue
            out.append(dict(cells=cells,
                            kind="exit" if any(i["kind"] == "exit" for i in infos) else "interior",
                            end=sorted({i["end"] for i in infos}, key=["room", "plug", "edge", "void", "drop"].index)[-1],
                            width=max(i["width"] for i in infos),
                            flank=min(i["flank"] for i in infos),
                            clear=min(clears) if clears else None,
                            layouts=layouts,
                            signals=signals))
        return out


def doorway_layouts(cands, lib):
    """Doorway candidate layouts of a library: (jambs at least 3 cells tall, flat thresholds)."""
    rows = cands.get(lib, [])
    return ({li for _, li, _, _, size, _ in rows if size[1] >= 3},
            {li for _, li, _, _, size, _ in rows if size[1] == 1})


def near(cells, box, pad=1):
    (x0, x1), (y0, y1), (z0, z1) = box
    return any(x0 - pad <= x <= x1 + pad and z0 - pad <= z <= z1 + pad and y0 - 2 <= y <= y1 for x, y, z in cells)


def doorways(grids, cands, zone_boxes, zone_cells):
    """Doorways found from geometry, checked against the cube-change zones and the doorway layouts."""
    print(f"\nDoorways (flanks >= {WALL_H} cells in a wall line, width <= {MAX_WIDTH}, depth <= {MAX_DEPTH} (exits {MAX_EXIT_DEPTH}); "
          f"exit = room on one side, invisible plug / void / edge on the other):")
    print("  per library: doorways (interior/exit) | in stubs | with cube zone | with candidate layout | interior without zone")
    per_lib = collections.defaultdict(collections.Counter)
    door_cells = collections.defaultdict(dict)
    found = {}
    for gi, g in sorted(grids.items()):
        jambs, thresholds = doorway_layouts(cands, g.lib)
        doors = DoorFinder(g, jambs, thresholds, zone_cells.get(gi, [])).doorways()
        for d in doors:
            zoned = any(near(d["cells"], box) for box in zone_cells.get(gi, []))
            k = per_lib[g.lib]
            k["doorways"] += 1; k[d["kind"]] += 1; k["stub"] += d["flank"] < TALL_H; k["zone"] += zoned
            k["layout"] += bool(d["layouts"] & jambs); k["interior_nozone"] += d["kind"] == "interior" and not zoned
            for c in d["cells"]:
                door_cells[gi][c] = "stub" if d["flank"] < TALL_H else d["kind"]
        for zi, box in enumerate(zone_cells.get(gi, [])):
            found[(gi, zi)] = any(near(d["cells"], box) for d in doors)
    for lib in sorted(per_lib):
        k = per_lib[lib]
        print(f"  lib {lib:2d}: {k['doorways']:4d} ({k['interior']:3d}/{k['exit']:3d}) | {k['stub']:3d} | {k['zone']:3d} | {k['layout']:3d} | {k['interior_nozone']:3d}")
    print(f"  cube zones with a doorway: {sum(found.values())}/{len(found)}")
    return door_cells, found


def arch(lo):
    """True when a layout has an opening under a solid top with solid jambs: a door frame."""
    sx, sy, sz = lo["size"]
    sh = lambda x, y, z: lo["blocks"][z * sx * sy + y * sx + x][0]
    holes = solid_cols = 0
    for x in range(sx):
        for z in range(sz):
            col = [sh(x, y, z) for y in range(sy)]
            if col[0] == 0 and any(col[3:]) and not any(col[:3]):
                holes += 1
            elif all(col[:3]):
                solid_cols += 1
    return holes > 0 and solid_cols >= 2


def render(libs, cands, out):
    from PIL import Image, ImageDraw
    os.makedirs(out, exist_ok=True)
    ress = entry(GAME + "RESS.HQR", 0)
    pal = [tuple(ress[i * 3:i * 3 + 3]) + (255,) for i in range(256)]
    cache = {}

    def sprite(brick):
        if brick not in cache:
            d = entry(BKG, BRICK_START + brick - 1)
            img = Image.new("RGBA", (48, 38))
            px = img.load(); lines, ox, oy = d[1], d[2], d[3]; at = 4
            for line in range(lines):
                runs = d[at]; at += 1; x = 0
                for _ in range(runs):
                    spec = d[at]; at += 1; n = (spec & 63) + 1; t = spec >> 6
                    for k in range(n):
                        c = d[at] if t == 2 else d[at + k] if t in (1, 3) else 0
                        if t and 0 <= x + ox < 48 and 0 <= line + oy < 38:
                            px[x + ox, line + oy] = pal[c]
                        x += 1
                    at += 1 if t == 2 else n if t in (1, 3) else 0
            cache[brick] = img
        return cache[brick]

    for lib, rows in cands.items():
        tiles = []
        for n, li, near, tot, (sx, sy, sz), _ in rows[:16]:
            lo = libs[lib][li]
            w = (sx + sz) * 24 + 8; h = (sx + sz) * 12 + sy * 15 + 40
            img = Image.new("RGBA", (w, h), (40, 40, 48, 255))
            order = sorted(((x, y, z) for x in range(sx) for y in range(sy) for z in range(sz)), key=lambda p: (p[0] + p[2], p[1]))
            for x, y, z in order:
                br = lo["blocks"][z * sx * sy + y * sx + x][2]
                if br and br != FORBIDDEN:
                    s = sprite(br)
                    img.alpha_composite(s, ((x - z) * 24 + sz * 24 - 20, (x + z) * 12 - y * 15 + sy * 15))
            ImageDraw.Draw(img).text((2, 2), f"L{li} z{n} {near}/{tot}", fill=(255, 255, 0, 255))
            tiles.append(img)
        if not tiles:
            continue
        cols = 6; cw = max(t.width for t in tiles); ch = max(t.height for t in tiles)
        sheet = Image.new("RGBA", (cw * min(cols, len(tiles)), ch * ((len(tiles) + cols - 1) // cols)), (20, 20, 24, 255))
        for i, t in enumerate(tiles):
            sheet.paste(t, ((i % cols) * cw, (i // cols) * ch))
        sheet.save(os.path.join(out, f"doors_lib{lib:02d}.png"))


if __name__ == "__main__":
    args = sys.argv[1:]
    png = args[args.index("-png") + 1] if "-png" in args else None
    nums = [int(a) for a in args if a.isdigit() and a != png]
    MAP_GRIDS.extend(nums)
    libs = {}
    survey(nums or range(GRM_START - GRID_START), libs, "-v" in args)
    doors(libs, png)
