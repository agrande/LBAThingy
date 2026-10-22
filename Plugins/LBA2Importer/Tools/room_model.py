"""Explicit room model of an LBA2 iso interior (prototype for the enclosure rebuild): rooms, the walls around each
room (per side), and the openings between rooms or out of the building. Top-down plan per grid to check the model
against the original before any geometry is generated from it.

Usage: room_model.py <grid> [grid ...] [-o <dir>]   (default dir Saved/LookDev/plans)

Model (grid axes: X, Z horizontal, Y up; the iso camera looks from +X +Z):
  Room     walkable floor (Headroom free cells above) joined by steps of one cell, split at doorways; its level is the
           most common floor height; its footprint adds the furniture among its floors (columns lower than a wall).
  Wall     a footprint column's neighbor that is a wall column (>= WALL_H solid cells above the room's level, visible
           or invisible collision), grouped per room into sides (-X, +X, -Z, +Z) and straight runs.
  Opening  doorways (survey_grids.DoorFinder), open joins to another room, open edges (drops, voids, grid edge).

Plan: columns shaded by their top height; room footprints colored per room (floors lighter); side edges drawn on the
cell borders: wall white (stub: drawn top below level+TALL_H, yellow), invisible collision cyan, low (1-2 cells)
orange, doorway blue, join to another room green, open edge red. Labels: room id, level, columns.
"""
import collections, colorsys, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S
from dump_scenes import parse as parse_scene

TOOLS = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.join(TOOLS, "..", "Source", "LBA2Editor", "Private", "LBA2ImporterSettings.cpp")
LOOKDEV = os.path.join(TOOLS, "..", "..", "..", "Saved", "LookDev")
HEADROOM = 4
WALL_H = 3
TALL_H = 5
TALL_WALL = 6        # solid cells above the floor that make a wall inside the footprint (furniture is lower)
MIN_ROOM = 16        # floor cells of a plateau that holds rooms
CUT_COLLISION = 2    # collision cells above the drawn top of a wall the original cut down
DIRS = {"-X": (-1, 0), "+X": (1, 0), "-Z": (0, -1), "+Z": (0, 1)}


def library_hints():
    """{lib: (jambs, thresholds, room height)} parsed from the settings seeds."""
    import re
    out = {}
    for m in re.finditer(r"Library\((\d+), TEXT\(\"[^\"]*\"\), (\d+), \d+, \{([^}]*)\}, \{([^}]*)\}", open(SETTINGS).read()):
        ids = lambda s: {int(v) for v in s.replace(" ", "").split(",") if v}
        out[int(m.group(1))] = (ids(m.group(3)), ids(m.group(4)), int(m.group(2)))
    return out


def zones_of(grid):
    """Cube-change zone boxes ((x0, x1), (y0, y1), (z0, z1)) in cells of the scenes using the grid."""
    out = []
    for scene, (is_island, gi) in enumerate(S.scene_map()[:222]):
        if is_island or gi != grid:
            continue
        try:
            s = parse_scene(scene)
        except Exception:
            continue
        for z in s["zones"]:
            if z["type"] == 0:
                x0, y0, z0, x1, y1, z1 = z["box"]
                out.append((tuple(sorted((x0 // 512, x1 // 512))), tuple(sorted((y0 // 256, y1 // 256))), tuple(sorted((z0 // 512, z1 // 512)))))
    return out


class Model:
    def __init__(self, g, wall_bricks=None):
        self.g = g
        self.wall_bricks = wall_bricks
        jambs, thresholds, self.room_height = library_hints().get(g.lib, ((), (), 9))
        self.doorways = S.DoorFinder(g, jambs, thresholds, zones_of(g.index)).doorways()
        self.door_cell = {c: i for i, d in enumerate(self.doorways) for c in d["cells"]}
        spaces = self._spaces()
        self.floors = {c for sp in spaces for c in sp}
        # Plateaus: the floors of a space around one level (steps of one cell, at most one from the level). Big ones
        # hold rooms; small ones are stairs and furniture tops, part of the rooms around them.
        plateaus = []
        for sp in spaces:
            left = set(sp)
            while left:
                lv = collections.Counter(y for _, y, _ in left).most_common(1)[0][0]
                seen = set()
                for seed in sorted(c for c in left if c[1] == lv):
                    if seed in seen:
                        continue
                    comp = self._flood(seed, left, lambda c, lv=lv: abs(c[1] - lv) <= 1)
                    seen |= comp
                    plateaus.append((lv, comp))
                left -= seen
        self.plateaus = plateaus
        big = [(lv, cells) for lv, cells in plateaus if len(cells) >= MIN_ROOM and self._inner(cells) >= 4]
        self.room_floor = {}            # floor column (x, z) -> level, of the big plateaus
        for lv, cells in big:
            for x, y, z in cells:
                self.room_floor.setdefault((x, z), []).append(lv)
        self.door_cols = {(x, z) for x, y, z in self.door_cell}
        self.rooms = []
        self.walls = {}                 # (x, z) -> (level, reach above it, drawn top above it)
        for lv, cells in big:
            self._rooms(lv, cells)
        # A doorway inside a room (the room goes around it) is part of it, unless it leads out.
        for d in self.doorways:
            if d["kind"] == "exit":
                continue
            touching = {ri for ri, r in enumerate(self.rooms) for x, y, z in d["cells"] for dx, dz in DIRS.values() if (x + dx, z + dz) in r["foot"]}
            if len(touching) == 1:
                r = self.rooms[touching.pop()]
                for x, y, z in d["cells"]:
                    r["foot"].add((x, z)); r["cols"][(x, z)] = y
                    self.door_cols.discard((x, z))
        # A gap between rooms that none reaches (a stairwell's top) belongs to the highest room around it.
        gaps = collections.defaultdict(set)
        for ri, r in enumerate(self.rooms):
            for (x, z) in r["foot"]:
                for dx, dz in DIRS.values():
                    n = (x + dx, z + dz)
                    if 0 <= n[0] < 64 and 0 <= n[1] < 64 and n not in self.door_cols and not any(n in o["foot"] for o in self.rooms):
                        gaps[n].add(ri)
        for n, around in gaps.items():
            if len(around) >= 2:
                top = max(around, key=lambda ri: self.rooms[ri]["level"])
                if not self.is_wall(n[0], n[1], self.rooms[top]["level"]):
                    self.rooms[top]["foot"].add(n)
        self.room_of = collections.defaultdict(list)
        for ri, r in enumerate(self.rooms):
            for c in r["foot"]:
                self.room_of[c].append(ri)
        for ri, r in enumerate(self.rooms):
            self._edges(ri, r)
        # Stairs: small plateaus between two room plateaus. Their steps keep the room height above them.
        index = {c: pi for pi, (_, cells) in enumerate(plateaus) for c in cells}
        bigs = {pi for pi, (lv, cells) in enumerate(plateaus) if (lv, cells) in big}
        self.stairs = {}
        for pi, (lv, cells) in enumerate(plateaus):
            if pi in bigs:
                continue
            near = {index[(x + dx, y + dy, z + dz)] for x, y, z in cells for dx, dz in DIRS.values() for dy in (-1, 0, 1)
                    if index.get((x + dx, y + dy, z + dz)) in bigs}
            if len({plateaus[o][0] for o in near}) >= 2:
                for x, y, z in cells:
                    self.stairs[(x, z)] = max(self.stairs.get((x, z), y), y)
        for r in self.rooms:
            r["ceiling"] = r["level"] + self.room_height
            r["ceilings"] = {c: max(r["ceiling"], self.stairs[c] + self.room_height) for c in r["foot"] if c in self.stairs}

    def _spaces(self):
        """Walkable spaces (survey_grids.Grid.spaces) without the tops of walls (floors drawn with a wall brick)."""
        g = self.g
        floors = g.floors()
        if self.wall_bricks:
            floors = {c for c in floors if int(g.brick[c]) not in self.wall_bricks}
        out = []; seen = set()
        for start in sorted(floors):
            if start in seen:
                continue
            comp = self._flood(start, floors, lambda c: True)
            seen |= comp
            if len(comp) >= 16:
                out.append(sorted(comp))
        return out

    @staticmethod
    def _inner(cells):
        return sum(all((x + dx, y, z + dz) in cells for dx, dz in DIRS.values()) for x, y, z in cells)

    @staticmethod
    def _flood(seed, cells, keep):
        comp = {seed}; todo = [seed]
        while todo:
            x, y, z = todo.pop()
            for dx, dz in DIRS.values():
                for dy in (0, 1, -1):
                    n = (x + dx, y + dy, z + dz)
                    if n in cells and n not in comp and keep(n):
                        comp.add(n); todo.append(n)
        return comp

    def run(self, x, y, z):
        """(solid cells stacked right above height y in column (x, z), top of the drawn ones above y (0: none))."""
        g = self.g; h = 0
        while y + 1 + h < 25 and g.solid[x, y + 1 + h, z]:
            h += 1
        drawn = [k for k in range(h) if g.brick[x, y + 1 + k, z]]
        return h, (max(drawn) + 1) if drawn else 0

    def reach(self, x, z, y):
        """Top of the wall surface rising from above height y in column (x, z): its solid run, continued by a
        neighbor column whose solid goes on from that top (the band of a camera-side wall over its stub)."""
        g = self.g
        h, _ = self.run(x, y, z)
        if h == 0:
            return y
        top = y + h
        for dx, dz in DIRS.values():
            nx, nz = x + dx, z + dz
            if 0 <= nx < 64 and 0 <= nz < 64 and top + 1 < 25 and g.solid[nx, top + 1, nz] and not g.solid[nx, y + 1, nz]:
                top = max(top, top + 1 + self.run(nx, top + 1, nz)[0])
        return top

    def is_wall(self, x, z, lv):
        """A wall column (wall_like) that continues a wall: a wall column next to it, or nothing standing on the
        floor behind it (the outside, a drop). A lone column inside a room is a pillar or furniture."""
        if not self.wall_like(x, z, lv):
            return False
        for dx, dz in DIRS.values():
            nx, nz = x + dx, z + dz
            if not (0 <= nx < 64 and 0 <= nz < 64) or self.wall_like(nx, nz, lv):
                return True
            if not self.g.solid[nx, lv:lv + 4, nz].any():
                return True
        return False

    def wall_like(self, x, z, lv):
        """A wall rises from the floor level lv in column (x, z): at least WALL_H solid cells, reaching TALL_WALL
        (in itself or through the band over it), or with collision going on above what is drawn (cut down)."""
        h, top = self.run(x, lv, z)
        if h < WALL_H:
            return False
        if h - top >= CUT_COLLISION:
            return True
        if self.wall_bricks:
            drawn = [int(self.g.brick[x, y, z]) for y in range(lv + 1, lv + 1 + h) if self.g.brick[x, y, z]]
            if not drawn or 2 * sum(b in self.wall_bricks for b in drawn) >= len(drawn):
                return True
        # A tall column with the outside behind it is a wall, whatever stands at its foot (a bathtub, curtains).
        if self.reach(x, z, lv) - lv < TALL_WALL:
            return False
        return not self.wall_bricks or any(self.outside(x + dx, z + dz, lv) for dx, dz in DIRS.values())

    def outside(self, x, z, lv):
        return not (0 <= x < 64 and 0 <= z < 64) or not self.g.solid[x, lv:lv + 4, z].any()

    def _rooms(self, lv, cells):
        """Rooms of a plateau at level lv: its floors and the furniture among them (columns with something standing
        on the floor, not walls), not through doorways nor other plateaus' floors, split into connected parts."""
        g = self.g
        floor_y = {}
        for x, y, z in cells:
            if (x, z) not in self.door_cols:
                floor_y[(x, z)] = y

        def passable(c):
            if not (0 <= c[0] < 64 and 0 <= c[1] < 64) or c in self.door_cols:
                return False
            if c in floor_y:
                return True
            if any(l != lv for l in self.room_floor.get(c, ())):
                return False
            if self.is_wall(c[0], c[1], lv):
                self.walls.setdefault(c, lv)
                return False
            return bool(g.solid[c[0], lv + 1, c[1]])

        seen = set()
        for start in sorted(floor_y):
            if start in seen:
                continue
            foot = {start}; todo = [start]
            while todo:
                x, z = todo.pop()
                for dx, dz in DIRS.values():
                    n = (x + dx, z + dz)
                    if n not in foot and passable(n):
                        foot.add(n); todo.append(n)
            seen |= foot
            fl = {c: floor_y[c] for c in foot if c in floor_y}
            if len(fl) < 4:
                continue
            self.rooms.append(dict(foot=foot, cols=fl, level=lv, levels=collections.Counter(fl.values())))

    def _edges(self, ri, r):
        g = self.g; lv = r["level"]
        edges = []     # (col, dir, kind, info)
        for (x, z) in r["foot"]:
            for d, (dx, dz) in DIRS.items():
                n = (x + dx, z + dz)
                if n in r["foot"]:
                    continue
                if not (0 <= n[0] < 64 and 0 <= n[1] < 64):
                    edges.append(((x, z), d, "edge", 0))
                elif n in self.door_cols:
                    edges.append(((x, z), d, "door", 0))
                elif self.is_wall(n[0], n[1], lv):
                    h, top = self.run(n[0], lv, n[1])
                    reach = self.reach(n[0], n[1], lv) - lv
                    edges.append(((x, z), d, "invis" if top == 0 else ("stub" if top < TALL_H else "wall"), reach))
                elif any(o != ri for o in self.room_of.get(n, ())):
                    o = next(o for o in self.room_of[n] if o != ri)
                    edges.append(((x, z), d, "join" if abs(self.rooms[o]["level"] - lv) <= 1 else "step", o))
                elif n in self.walls:
                    edges.append(((x, z), d, "rise", 0))     # another room's wall, a step from this floor
                else:
                    edges.append(((x, z), d, "open", 0))
        r["edges"] = edges
        r["sides"] = {d: collections.Counter(k for _, dd, k, _ in edges if dd == d) for d in DIRS}

    def report(self):
        print(f"grid {self.g.index} lib {self.g.lib}: {len(self.rooms)} rooms, {len(self.doorways)} doorways")
        for ri, r in enumerate(self.rooms):
            lv = ",".join(f"{y}:{n}" for y, n in sorted(r["levels"].items()))
            sides = "  ".join(f"{d} " + "/".join(f"{k}{n}" for k, n in c.most_common()) for d, c in r["sides"].items())
            xs = [x for x, _ in r["foot"]]; zs = [z for _, z in r["foot"]]
            print(f"  R{ri}: level {r['level']} ceiling {r['ceiling']} stairs {sorted(set(r['ceilings'].values()))} floors {len(r['cols'])} foot {len(r['foot'])} box x{min(xs)}-{max(xs)} z{min(zs)}-{max(zs)} levels [{lv}]")
            print(f"       {sides}")
        for di, d in enumerate(self.doorways):
            c = sorted(d["cells"])
            print(f"  D{di}: {d['kind']}/{d['end']} at {c[0]} w{d['width']} clear {d['clear']} {'+'.join(d['signals'])}")

    def plan(self, out, scale=12):
        from PIL import Image, ImageDraw
        g = self.g; s = scale
        img = Image.new("RGB", (64 * s, 64 * s + 20), (12, 12, 16)); dr = ImageDraw.Draw(img)
        for x in range(64):
            for z in range(64):
                ys = np.nonzero(g.solid[x, :, z])[0]
                if len(ys):
                    v = 30 + int(ys[-1]) * 5
                    dr.rectangle([x * s, z * s, x * s + s - 1, z * s + s - 1], fill=(v, v, v))
        for ri, r in enumerate(self.rooms):
            rgb = colorsys.hsv_to_rgb((ri * 0.618) % 1, 0.55, 0.75)
            base = tuple(int(c * 255) for c in rgb)
            for (x, z) in r["foot"]:
                y = r["cols"].get((x, z))
                if y is None:
                    col = tuple(c // 2 for c in base)
                else:
                    k = 1.0 + 0.12 * (y - r["level"])
                    col = tuple(max(0, min(255, int(c * k))) for c in base)
                dr.rectangle([x * s, z * s, x * s + s - 1, z * s + s - 1], fill=col)
        for (x, z), lv in self.walls.items():
            h, top = self.run(x, lv, z)
            v = 120 + min(self.reach(x, z, lv) - lv, 20) * 6
            dr.rectangle([x * s + 1, z * s + 1, x * s + s - 2, z * s + s - 2], fill=(v, v, v) if top >= TALL_H else ((v, v, 0) if top else (0, v, v)))
        for (x, y, z), di in self.door_cell.items():
            dr.rectangle([x * s + 2, z * s + 2, x * s + s - 3, z * s + s - 3], fill=(40, 90, 255))
        colors = dict(wall=(255, 255, 255), stub=(255, 220, 40), invis=(40, 230, 255), inner=(255, 140, 30), band=(200, 80, 255),
                      door=(60, 110, 255), join=(60, 230, 80), step=(230, 60, 230), rise=(160, 60, 255), open=(255, 40, 40), edge=(255, 40, 40))
        for r in self.rooms:
            for (x, z), d, kind, _ in r["edges"]:
                dx, dz = DIRS[d]
                if dx:
                    ex = x * s + (s - 1 if dx > 0 else 0)
                    seg = [ex, z * s, ex, z * s + s - 1]
                else:
                    ez = z * s + (s - 1 if dz > 0 else 0)
                    seg = [x * s, ez, x * s + s - 1, ez]
                dr.line(seg, fill=colors[kind], width=2)
        for ri, r in enumerate(self.rooms):
            xs = [x for x, _ in r["cols"]]; zs = [z for _, z in r["cols"]]
            cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
            dr.text((cx * s - 10, cz * s - 6), f"R{ri} L{r['level']}\nC{r['ceiling']}", fill=(0, 0, 0))
            for (x, z), c in r["ceilings"].items():
                dr.text((x * s + 1, z * s), str(c), fill=(255, 255, 255))
        dr.text((4, 64 * s + 4), f"grid {g.index} lib {g.lib}: {len(self.rooms)} rooms {len(self.doorways)} doorways", fill=(255, 255, 0))
        os.makedirs(out, exist_ok=True)
        path = os.path.join(out, f"plan_{g.index:03d}.png")
        img.save(path)
        return path


def learn_wall_bricks(lib, libs, verbose=False):
    """Bricks of a library's walls: drawn in the back-most wall columns around rooms (nothing stands on the floor
    behind them), and not in free-standing furniture (room columns with no wall next to them), found with the
    height rules over every grid of the library."""
    wall = collections.Counter(); furniture = collections.Counter(); floor = collections.Counter()
    for gi in range(S.GRM_START - S.GRID_START):
        if S.load_grid(gi)[0] != lib:
            continue
        g = S.Grid(gi, libs)
        m = Model(g)
        for r in m.rooms:
            lv = r["level"]
            floor.update(int(g.brick[x, y, z]) for (x, z), y in r["cols"].items() if g.brick[x, y, z])
            for (x, z), d, kind, _ in r["edges"]:
                if kind not in ("wall", "stub"):
                    continue
                dx, dz = DIRS[d]
                nx, nz = x + dx, z + dz
                bx, bz = nx + dx, nz + dz
                if 0 <= bx < 64 and 0 <= bz < 64 and g.solid[bx, lv + 1:lv + 4, bz].any():
                    continue
                h, _ = m.run(nx, lv, nz)
                wall.update(int(g.brick[nx, y, nz]) for y in range(lv + 1, lv + 1 + h) if g.brick[nx, y, nz])
            for (x, z) in r["foot"]:
                if (x, z) in r["cols"]:
                    continue
                if any((x + dx, z + dz) in m.walls or (x + dx, z + dz) not in r["foot"] for dx, dz in DIRS.values()):
                    continue
                furniture.update(int(g.brick[x, y, z]) for y in range(lv + 1, min(25, lv + 6)) if g.brick[x, y, z])
    bricks = {b for b, n in wall.items() if n >= 2 and n >= 4 * (furniture[b] + floor[b])}
    if verbose:
        mixed = sorted(b for b in wall if b not in bricks and wall[b] >= 2)
        print(f"lib {lib}: {len(bricks)} wall bricks, {len(furniture)} furniture bricks, mixed {len(mixed)}")
    return bricks


_WALL_BRICKS = {}


def wall_bricks(lib, libs):
    if lib not in _WALL_BRICKS:
        _WALL_BRICKS[lib] = learn_wall_bricks(lib, libs)
    return _WALL_BRICKS[lib]


if __name__ == "__main__":
    args = sys.argv[1:]
    out = args[args.index("-o") + 1] if "-o" in args else os.path.join(LOOKDEV, "plans")
    libs = {}
    for a in args:
        if a.isdigit():
            g = S.Grid(int(a), libs)
            m = Model(g, wall_bricks(g.lib, libs))
            m.report()
            print("  ->", m.plan(out))
