"""Rectangular room plan of an LBA2 iso interior: the prototype of the enclosure's model.

Usage: room_plan.py <grid> [grid ...] [-o <dir>]    (plans in Saved/LookDev/plans/plan_NNN.png and .txt)

The plan is the data the enclosure is built from. It is made of human architectural concepts, each found by one
explicit rule, applied in this order (grid axes: X, Z horizontal, Y up; the iso camera looks from +X +Z):

  Levels      floor heights with a connected floor of at least MIN_LEVEL_FLOORS cells, MIN_INNER_FLOORS of them with
              floor on all four sides: the storeys of the building (smaller floors are furniture tops and steps;
              strips one cell wide are ledges and the tops of walls).
  Stairs      R1 STAIRS: floors at no level's height that climb one cell at a time from a floor of one level to a
              floor of another: a flight of stairs (one step up a few cells is a flight too).
  Columns     at each level, each grid column is one of
                FLOOR      walkable floor at the level,
                STAIRS     a flight's column,
                FURNITURE  something standing on the level's floor that is not a wall (furniture, a step, a pillar):
                           its first drawn layout starts at the floor or above (a wardrobe on a lower storey is
                           no furniture of the storey above, though it reaches that high), and not another
                           storey's floor (a level that holds no room is no storey: the plan is made again
                           without it, so a machine whose top is such a level is furniture),
                WALL       see "What is a wall" below,
                OTHER      nothing of this level: a drop, the void, another level's floor, the grid's edge.
  Posts       a WALL column in no straight run of MIN_WALL_RUN wall columns is a post or a pillar: FURNITURE if it
              stands on the level's floor, else OTHER (the end of a lower storey's wall).
  Wall lines  R2 GAP: the floor between two walls in line with each other (a wall that stops and goes on) is an
              opening in that wall's line: a doorway, a window bay, an arch. It is no room's.
              R3 PARTITION: a wall with at least MIN_PARTITION columns of its own (past the walls it meets) that ends
              inside the floor (its last column meets no wall and no wall's line) goes on as a line across the floor
              to the next wall: the opening beside the partition's end is no room's either. Shorter returns are
              door reveals, pilasters and the sides of niches.
  Rooms       R4 RECTANGLE: a room is the largest rectangle of FLOOR and FURNITURE columns off the wall lines, at
              least MIN_ROOM_WIDTH columns wide, holding at least MIN_FLOORS floor cells, and
                walled     a WALL column along one of its sides, or a step to a split level beside it (with
                           neither, it is the top of something: a roof, wall tops, a big piece of furniture),
                no roof    not more than half of its floor on the walls of a lower storey (a roof, the top of a
                           hull or of a wall; an upper storey stands over the rooms below),
                on storey  more than half of its floor on the level's connected floor (not a patch at the same
                           height: a ledge, a rim),
                enclosed   not more than half of its border open, nothing walkable and no wall there (open ground);
              then the largest among the columns left, and so on. A floor that is no rectangle (an L) is several
              rooms sharing open sides: one space.
  Connectors  the columns no room took: FLOOR and FURNITURE columns of each level grouped by contact, joined by the
              flights of stairs they lead onto. Each is, by the rooms it touches:
                STAIRS   rooms of two levels: stairs between storeys, with the landings and doorways at their ends,
                PASSAGE  two rooms of one level: the opening between them (a door or an arch by its width),
                EXIT     one room, holding a scene-change zone: the way out of the building,
                ALCOVE   one room: a niche, part of that room's space,
                POCKET   no room: out of reach, ignored.
  Sides       each room's four sides, column by column, what lies past the rectangle:
                WALL, OPEN_SIDE (a room of the same level with no wall between: one space, as the two rectangles of
                an L-shaped room), a connector (STAIRS, PASSAGE, EXIT, ALCOVE), STEP (a room or floor of another
                level), OPEN (a drop, the void, the grid's edge: nothing to build).
  Spaces      rooms joined by open sides, or by a step to a split level (at most HEADROOM up or down, no wall
              between: a sunken lounge, a dais): one space under one ceiling (an L-shaped room, an irregular hall).
  Ceilings    the first cell over a space's air: its highest room's level + the room height (cells of air) + 1,
              R5 DOUBLE HEIGHT: or the ceiling of a higher storey that overlooks it: that storey's floor (its edge,
              railings included), more than HEADROOM cells up (one can stand under it), borders a floor column of
              the space that is open from the space's floor up past it (a gallery, a mezzanine, a landing over a
              hall). Storeys closer than that are split levels (see Spaces).
              A connector's: the highest of its rooms'. Under a gallery or a lintel the geometry stops at the
              first solid cell above the floor.

What is a wall (the only place where the iso data's tricks are read):
  a column with at least WALL_H solid cells over the level (through a window gap), that is one of
    cut        collision goes on above what is drawn (the original cut camera-side walls down and kept the collision),
               or what is drawn is topped by a cut-wall frame (the caps and curved rims the original put on the walls
               it cut down: the library's CutFrames),
    invisible  nothing drawn at all (the plugs behind exits, hidden walls),
    masonry    drawn mostly with the library's wall bricks (learned: bricks of back walls, not of furniture),
    exterior   reaching TALL_WALL cells with the outside (nothing) behind it,
    jamb       next to an exit,
  and that goes on: a wall column next to it, or the outside behind it (a lone column in a room is a pillar).
    tall       (only while learning the wall bricks, see below) reaching TALL_WALL cells,
  Furniture against a wall (a bookcase, a lamp, a bed's post) is lower than the wall behind it: a masonry or
  exterior column met from a floor is furniture when the wall-like columns going on away from that floor end,
  within AGAINST_DEPTH, at a drawn, taller wall that is part of a wall across. Cut and invisible columns are walls
  the original hid: never furniture.

Learning the wall bricks (once per library, learn_wall_bricks): plans of every grid of the library made with the
height rules alone (cut, invisible, jamb, tall); then a brick is a wall brick when it is drawn at least twice in the
back-most wall columns on the rooms' sides (nothing stands on the floor behind them) and four times as often as in
free-standing furniture (furniture columns with the room's floor all around) and floors.
"""
import collections, colorsys, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S
from dump_scenes import parse as parse_scene
from survey_walls import blank

TOOLS = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.join(TOOLS, "..", "Source", "LBA2Editor", "Private", "LBA2ImporterSettings.cpp")
LOOKDEV = os.path.join(TOOLS, "..", "..", "..", "Saved", "LookDev")

HEADROOM = 4          # free cells over a walkable floor (survey_grids.Grid.floors)
MIN_LEVEL_FLOORS = 16 # connected floor cells that make a level (a storey)
MIN_INNER_FLOORS = 4  # cells of a level's floor with floor all around
MIN_FLOORS = 12       # floor cells of a room
MIN_ROOM_WIDTH = 2    # columns across a room (one column is a ledge or a corridor: a connector)
WALL_H = 3            # solid cells over the level that make a wall
TALL_WALL = 6         # solid cells that make an exterior wall whatever it is drawn with
CUT_COLLISION = 2     # collision cells above the drawn top of a cut wall
AGAINST_DEPTH = 3     # furniture columns in front of a wall
WINDOW_GAP = 2        # empty cells of a window in a wall
MIN_WALL_RUN = 3      # wall columns in a straight run that make a wall (shorter: a post)
MIN_PARTITION = 4     # columns of its own (in no other wall) that make a wall a partition, whose line divides rooms
DOOR_WIDTH = 2        # passages up to this wide are doors, wider ones arches
DIRS = {"-X": (-1, 0), "+X": (1, 0), "-Z": (0, -1), "+Z": (0, 1)}
FLOOR, STAIRS, FURNITURE, WALL, OTHER = "floor", "stairs", "furniture", "wall", "other"


def cut_frames():
    """{lib: CutFrames layouts} parsed from the settings seeds."""
    import re
    out = {}
    for m in re.finditer(r"Library\((\d+), TEXT\(\"[^\"]*\"\), \d+, \d+, \{([^}]*)\}", open(SETTINGS).read()):
        out[int(m.group(1))] = {int(v) for v in m.group(2).replace(" ", "").split(",") if v}
    return out


def room_heights():
    """{lib: room height (cells of air)} parsed from the settings seeds."""
    import re
    return {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"Library\((\d+), TEXT\(\"[^\"]*\"\), (\d+), \d+,", open(SETTINGS).read())}


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


def inside(x, z):
    return 0 <= x < 64 and 0 <= z < 64


def flood(seeds, keep, neighbors):
    """Component of seeds through neighbors(cell) where keep(cell)."""
    comp = set(seeds); todo = list(seeds)
    while todo:
        for n in neighbors(todo.pop()):
            if n not in comp and keep(n):
                comp.add(n); todo.append(n)
    return comp


def plan_neighbors(c):
    return [(c[0] + dx, c[1] + dz) for dx, dz in DIRS.values()]


def step_neighbors(c):
    """Floor cells reached from floor cell c in one step: sideways, one up or one down."""
    x, y, z = c
    return [(x + dx, y + dy, z + dz) for dx, dz in DIRS.values() for dy in (0, 1, -1)]


class Columns:
    """The classification of a grid's columns at one level, and the wall rules (wall_bricks None: learning)."""

    def __init__(self, g, level, floors, stairs, room_floor_levels, wall_bricks, exit_cols, roomless=()):
        self.g, self.level, self.wall_bricks, self.exit_cols = g, level, wall_bricks, exit_cols
        self.learning = wall_bricks is None
        self.frames = cut_frames().get(g.lib, set())
        self.cls = {}
        self.why = {}                               # (x, z) -> the wall rule that fired
        for x in range(64):
            for z in range(64):
                c = (x, z)
                if c in floors:
                    self.cls[c] = FLOOR
                elif c in stairs:
                    self.cls[c] = STAIRS
                elif self.is_wall(x, z):
                    self.cls[c] = WALL
                elif self.stands_on_floor(x, z) and not any(l != level and l not in roomless for l in room_floor_levels.get(c, ())):
                    self.cls[c] = FURNITURE
                else:
                    self.cls[c] = OTHER
        # Furniture against a wall: judged from the floors around, once every column is classed.
        for c in [c for c, k in self.cls.items() if k == WALL and self.why[c] in ("masonry", "exterior") and not self.learning]:
            for d, (dx, dz) in DIRS.items():
                if self.cls.get((c[0] - dx, c[1] - dz)) == FLOOR and self.against(c[0], c[1], d):
                    self.cls[c] = FURNITURE
                    self.why[c] = "against"
                    break
        # Posts: wall columns in no straight run of MIN_WALL_RUN.
        self.runs = self.wall_runs()
        in_run = {c for run in self.runs for c in run}
        for c in [c for c, k in self.cls.items() if k == WALL and c not in in_run]:
            self.cls[c] = FURNITURE if self.stands_on_floor(*c) else OTHER
            self.why[c] = "post"
        self.runs = self.wall_runs()

    def stands_on_floor(self, x, z):
        """Something stands on the level's floor in the column: the cell over the floor is solid, and the first drawn
        piece above the floor begins at the floor or above (invisible collision under it is its hidden plinth)."""
        g, y = self.g, self.level
        if y + 1 >= 25 or not g.solid[x, y + 1, z]:
            return False
        for k in range(y + 1, 25):
            if not g.solid[x, k, z]:
                return True
            if g.layout[x, k, z] >= 0:
                return g.anchor(x, k, z)[1][1] >= y
        return True

    def wall_runs(self):
        """Maximal straight runs of at least MIN_WALL_RUN wall columns, along X and along Z: [[(x, z), ...]]."""
        out = []
        for axis in (0, 1):
            for a in range(64):
                run = []
                for b in range(65):
                    c = (b, a) if axis == 0 else (a, b)
                    if b < 64 and self.cls[c] == WALL:
                        run.append(c)
                    else:
                        if len(run) >= MIN_WALL_RUN:
                            out.append(run)
                        run = []
        return out

    def run(self, x, z):
        """(solid cells stacked over the level in the column, through window gaps; top of the drawn ones, 0: none)."""
        g = self.g; y = self.level; k = y + 1; end = y
        while k < 25:
            if g.solid[x, k, z]:
                end = k; k += 1
                continue
            if k == y + 1 or not any(k + j < 25 and g.solid[x, k + j, z] for j in range(1, WINDOW_GAP + 1)):
                break
            k += 1
        drawn = [j for j in range(y + 1, end + 1) if g.brick[x, j, z]]
        return end - y, (max(drawn) - y) if drawn else 0

    def reach(self, x, z):
        """Top of the wall surface over the level in the column: its run, continued by the band next door that tops a
        camera-side wall's stub."""
        g = self.g; y = self.level
        h, _ = self.run(x, z)
        if h == 0:
            return y
        top = y + h
        for dx, dz in DIRS.values():
            nx, nz = x + dx, z + dz
            if inside(nx, nz) and top + 1 < 25 and g.solid[nx, top + 1, nz] and not g.solid[nx, y + 1, nz]:
                top = max(top, top + 1 + self.run_from(nx, nz, top + 1))
        return top

    def run_from(self, x, z, y):
        h = 0
        while y + 1 + h < 25 and self.g.solid[x, y + 1 + h, z]:
            h += 1
        return h

    def outside(self, x, z):
        return not inside(x, z) or not self.g.solid[x, self.level:self.level + 4, z].any()

    def wall_like(self, x, z):
        """The wall rules; the name of the rule that fired, or None."""
        h, top = self.run(x, z)
        if h < WALL_H:
            return None
        if h - top >= CUT_COLLISION or (top and int(self.g.layout[x, self.level + top, z]) in self.frames):
            return "cut"
        if any((x + dx, z + dz) in self.exit_cols for dx, dz in DIRS.values()):
            return "jamb"
        drawn = [int(self.g.brick[x, y, z]) for y in range(self.level + 1, self.level + 1 + h) if self.g.brick[x, y, z]]
        if not drawn:
            return "invisible"
        if self.learning:
            return "tall" if max(self.reach(x, z) - self.level, h) >= TALL_WALL else None
        if 2 * sum(b in self.wall_bricks for b in drawn) >= len(drawn):
            return "masonry"
        if max(self.reach(x, z) - self.level, h) >= TALL_WALL and any(self.outside(x + dx, z + dz) for dx, dz in DIRS.values()):
            return "exterior"
        return None

    def is_wall(self, x, z):
        rule = self.wall_like(x, z)
        if not rule:
            return False
        for dx, dz in DIRS.values():
            nx, nz = x + dx, z + dz
            if not inside(nx, nz) or self.wall_like(nx, nz) or self.outside(nx, nz):
                self.why[(x, z)] = rule
                return True
        self.why[(x, z)] = "pillar"
        return False

    def against(self, x, z, d):
        """Column (x, z), met from a floor going d, stands against a wall: the wall-like columns going on in d end
        within AGAINST_DEPTH at a drawn wall, part of a wall across d, taller than every column before it."""
        dx, dz = DIRS[d]
        chain = [(x, z)]
        while True:
            nx, nz = chain[-1][0] + dx, chain[-1][1] + dz
            if not inside(nx, nz) or not self.wall_like(nx, nz):
                break
            chain.append((nx, nz))
            if len(chain) > AGAINST_DEPTH + 1:
                return False
        if len(chain) < 2:
            return False
        wx, wz = chain[-1]
        g = self.g
        if not any(g.brick[wx, y, wz] and not blank(int(g.brick[wx, y, wz])) for y in range(self.level + 1, 25)):
            return False
        if not any(inside(wx + sz, wz + sx) and self.wall_like(wx + sz, wz + sx) for sx, sz in ((dx, dz), (-dx, -dz))):
            return False
        top = self.reach(wx, wz)
        return all(self.reach(cx, cz) < top for cx, cz in chain[:-1])


def maximal_rectangles(free):
    """All maximal rectangles (x0, x1, z0, z1, inclusive) of a 64x64 bool array."""
    out = []
    heights = np.zeros(64, int)
    for z in range(64):
        heights = np.where(free[:, z], heights + 1, 0)
        stack = []      # (start x, height)
        for x in range(65):
            h = heights[x] if x < 64 else 0
            start = x
            while stack and stack[-1][1] >= h:
                sx, sh = stack.pop()
                if sh > 0 and (not stack or stack[-1][1] < sh):
                    out.append((sx, x - 1, z - sh + 1, z))
                start = sx
            if not stack or stack[-1][1] < h:
                stack.append((start, h))
    # Only the rectangles that cannot grow down either (maximal in Z too).
    return [r for r in out if r[3] == 63 or not free[r[0]:r[1] + 1, r[3] + 1].all()]


class Plan:
    def __init__(self, g, bricks, zones):
        self.g = g
        self.room_height = room_heights().get(g.lib, 9)
        self.zones = zones
        floors = g.floors()
        by_height = collections.defaultdict(set)
        for x, y, z in floors:
            by_height[y].add((x, z))
        self.floor_counts = {y: len(c) for y, c in by_height.items()}

        # Levels.
        self.levels = []
        self.room_floor_levels = collections.defaultdict(list)   # (x, z) -> levels whose connected floor it is in
        for y in sorted(by_height):
            seen = set()
            for start in sorted(by_height[y]):
                if start in seen:
                    continue
                comp = flood([start], lambda c, y=y: c in by_height[y], plan_neighbors)
                seen |= comp
                inner = sum(all(n in comp for n in plan_neighbors(c)) for c in comp)
                if len(comp) >= MIN_LEVEL_FLOORS and inner >= MIN_INNER_FLOORS:
                    for c in comp:
                        self.room_floor_levels[c].append(y)
                    if y not in self.levels:
                        self.levels.append(y)
        level_floor = {(x, y, z) for (x, z), ys in self.room_floor_levels.items() for y in ys}

        # R1 STAIRS.
        self.flights = []       # dict(cells, levels)
        off_level = {c for c in floors if c[1] not in self.levels}
        seen = set()
        for start in sorted(off_level):
            if start in seen:
                continue
            cells = flood([start], lambda c: c in off_level, step_neighbors)
            seen |= cells
            levels = {n[1] for c in cells for n in step_neighbors(c) if n in level_floor}
            if len(levels) >= 2:
                self.flights.append(dict(cells=cells, levels=sorted(levels)))
        stair_cols = {(x, z) for f in self.flights for x, _, z in f["cells"]}

        exit_cols = set()
        for (x0, x1), (y0, y1), (z0, z1) in zones:
            exit_cols |= {(x, z) for x in range(x0, x1 + 1) for z in range(z0, z1 + 1) if inside(x, z)}
        # Columns and rooms of each level; again without the levels that hold no room (no storeys: machine tops,
        # ledges), which then keep no furniture off the storeys below them.
        self.roomless = set()
        while True:
            self.columns = {}
            self.rooms = []     # dict(level, box=(x0, x1, z0, z1), floors, furniture, sides, ceiling)
            for level in self.levels:
                cols = Columns(g, level, by_height[level], stair_cols, self.room_floor_levels, bricks, exit_cols, self.roomless)
                self.columns[level] = cols
                self._rooms(level, cols)
            roomless = {l for l in self.levels if not any(r["level"] == l for r in self.rooms)}
            if roomless <= self.roomless:
                break
            self.roomless |= roomless
        self.room_at = {}       # (level, (x, z)) -> room
        for ri, r in enumerate(self.rooms):
            x0, x1, z0, z1 = r["box"]
            r["ceiling"] = min(r["level"] + self.room_height + 1, 25)
            for x in range(x0, x1 + 1):
                for z in range(z0, z1 + 1):
                    self.room_at[(r["level"], (x, z))] = ri
        self._connectors(stair_cols)
        for r in self.rooms:
            self._sides(r)
        self._spaces()

    def _rooms(self, level, cols):
        """R2 GAP, R3 PARTITION, then R4 RECTANGLE."""
        free = np.zeros((64, 64), bool)
        floor = np.zeros((64, 64), bool)
        for (x, z), k in cols.cls.items():
            free[x, z] = k in (FLOOR, FURNITURE)
            floor[x, z] = k == FLOOR
        cols.cuts = set()
        cols.gaps = set()
        in_run = collections.Counter(c for run in cols.runs for c in run)
        cols.extensions = []
        # Each wall end that meets no other wall: the free columns in its line, and what stops them.
        ends = []
        for run in cols.runs:
            (ax, az), (bx, bz) = run[0], run[-1]
            d = (0, 1) if ax == bx else (1, 0)
            for (x, z), (dx, dz) in (((ax, az), (-d[0], -d[1])), ((bx, bz), d)):
                if in_run[(x, z)] > 1:
                    continue
                end = (x, z)
                line = []
                x, z = x + dx, z + dz
                while inside(x, z) and free[x, z]:
                    line.append((x, z))
                    x, z = x + dx, z + dz
                if line:
                    ends.append((run, end, (dx, dz), line, (x, z)))
        # R2 GAP: the line reaches a wall in a run along the same axis.
        for run, end, d, line, stop in ends:
            if inside(*stop) and any(stop in other and (other[0][0] == other[-1][0]) == (run[0][0] == run[-1][0]) for other in cols.runs):
                cols.gaps.update(line)
                cols.extensions.append(("gap", run[0], run[-1], end, line))
        # R3 PARTITION: a partition's line, up to a gap in another wall's line.
        for run, end, d, line, stop in ends:
            if line[0] in cols.gaps or sum(in_run[c] == 1 for c in run) < MIN_PARTITION:
                continue
            cut = []
            for c in line:
                if c in cols.gaps:
                    break
                cut.append(c)
            if cut:
                cols.cuts.update(cut)
                cols.extensions.append(("partition", run[0], run[-1], end, cut))
        cols.cuts |= cols.gaps
        for x, z in cols.cuts:
            free[x, z] = False
        # R4 RECTANGLE.
        wall = np.zeros((66, 66), bool)       # padded by one column around the grid
        for (x, z), k in cols.cls.items():
            wall[x + 1, z + 1] = k == WALL

        lower = [self.columns[l] for l in self.levels if l < level]

        def roof(x0, x1, z0, z1):
            fl = [(x, z) for x in range(x0, x1 + 1) for z in range(z0, z1 + 1) if floor[x, z]]
            return 2 * sum(any(c.cls[f] == WALL for c in lower) for f in fl) > len(fl)

        def off_storey(x0, x1, z0, z1):
            fl = [(x, z) for x in range(x0, x1 + 1) for z in range(z0, z1 + 1) if floor[x, z]]
            return 2 * sum(level in self.room_floor_levels.get(f, ()) for f in fl) <= len(fl)

        def open_ground(x0, x1, z0, z1):
            border = [(x, z0 - 1) for x in range(x0, x1 + 1)] + [(x, z1 + 1) for x in range(x0, x1 + 1)]                 + [(x0 - 1, z) for z in range(z0, z1 + 1)] + [(x1 + 1, z) for z in range(z0, z1 + 1)]
            open_ = sum(not inside(*n) or (cols.cls[n] == OTHER and not self.room_floor_levels.get(n)) for n in border)
            return 2 * open_ > len(border)

        def walled(x0, x1, z0, z1):
            if wall[x0 + 1:x1 + 2, z0].any() or wall[x0 + 1:x1 + 2, z1 + 2].any() or wall[x0, z0 + 1:z1 + 2].any() or wall[x1 + 2, z0 + 1:z1 + 2].any():
                return True
            # Or a step to a split level: a floor at most HEADROOM up or down.
            border = [(x, z0 - 1) for x in range(x0, x1 + 1)] + [(x, z1 + 1) for x in range(x0, x1 + 1)]                 + [(x0 - 1, z) for z in range(z0, z1 + 1)] + [(x1 + 1, z) for z in range(z0, z1 + 1)]
            return any(l != level and abs(l - level) <= HEADROOM for n in border for l in self.room_floor_levels.get(n, ()))
        while True:
            best = None
            for x0, x1, z0, z1 in maximal_rectangles(free):
                n = int(floor[x0:x1 + 1, z0:z1 + 1].sum())
                if n < MIN_FLOORS or min(x1 - x0, z1 - z0) + 1 < MIN_ROOM_WIDTH or not walled(x0, x1, z0, z1) or roof(x0, x1, z0, z1)                         or off_storey(x0, x1, z0, z1) or open_ground(x0, x1, z0, z1):
                    continue
                key = ((x1 - x0 + 1) * (z1 - z0 + 1), n, -x0, -z0)
                if best is None or key > best[0]:
                    best = (key, (x0, x1, z0, z1))
            if best is None:
                break
            x0, x1, z0, z1 = best[1]
            n = int(floor[x0:x1 + 1, z0:z1 + 1].sum())
            self.rooms.append(dict(level=level, box=best[1], floors=n, furniture=int(free[x0:x1 + 1, z0:z1 + 1].sum()) - n))
            free[x0:x1 + 1, z0:z1 + 1] = False

    def _connectors(self, stair_cols):
        """The columns no room took, grouped: per level by contact, joined through the flights they lead onto."""
        # Pieces: (level, columns) per level, and (None, columns) per flight.
        pieces = []
        for level in self.levels:
            cols = self.columns[level]
            left = {c for c, k in cols.cls.items() if k in (FLOOR, FURNITURE) and (level, c) not in self.room_at}
            seen = set()
            for start in sorted(left):
                if start not in seen:
                    comp = flood([start], lambda c: c in left, plan_neighbors)
                    seen |= comp
                    pieces.append((level, comp))
        for f in self.flights:
            pieces.append((None, {(x, z) for x, _, z in f["cells"]}))
        # A level's piece leads onto a flight where one of its floors is a step away from the flight's cells.
        parent = list(range(len(pieces)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i
        for fi, f in enumerate(self.flights):
            fp = len(pieces) - len(self.flights) + fi
            near = {n for c in f["cells"] for n in step_neighbors(c)}
            for pi, (level, comp) in enumerate(pieces):
                if level is not None and any((c[0], level, c[1]) in near and self.columns[level].cls[c] == FLOOR for c in comp):
                    parent[find(pi)] = find(fp)
        groups = collections.defaultdict(list)
        for pi in range(len(pieces)):
            groups[find(pi)].append(pieces[pi])

        self.connectors = []    # dict(kind, pieces, rooms, levels, width, ceiling)
        self.connector_at = {}  # (level or None, (x, z)) -> connector
        flight_level = {}       # flight column -> its floor heights
        for f in self.flights:
            for x, y, z in f["cells"]:
                flight_level.setdefault((x, z), set()).add(y)
        for members in groups.values():
            touching = collections.Counter()
            for level, comp in members:
                for c in comp:
                    for n in plan_neighbors(c):
                        if level is not None:
                            ri = self.room_at.get((level, n))
                            if ri is not None:
                                touching[ri] += 1
                        else:
                            for y in flight_level[c]:
                                for ly in (y - 1, y + 1):
                                    ri = self.room_at.get((ly, n))
                                    if ri is not None and self.columns[ly].cls[n] == FLOOR:
                                        touching[ri] += 1
            rooms = sorted(touching)
            levels = sorted({self.rooms[ri]["level"] for ri in rooms})
            exit_ = any(self.in_zone(c, level) for level, comp in members if level is not None for c in comp)
            kind = ("stairs" if len(levels) >= 2 else "passage" if len(rooms) >= 2 else "exit" if rooms and exit_
                    else "alcove" if rooms else "pocket")
            width = min(touching.values()) if kind == "passage" else 0
            ci = len(self.connectors)
            self.connectors.append(dict(kind=kind, pieces=members, rooms=rooms, levels=levels, width=width,
                                        ceiling=max((self.rooms[ri]["ceiling"] for ri in rooms), default=None)))
            for level, comp in members:
                for c in comp:
                    self.connector_at[(level, c)] = ci

    def _spaces(self):
        """Spaces, then R5 DOUBLE HEIGHT, then the connectors' ceilings."""
        self.spaces = []        # dict(rooms, level, ceiling, overlooked_by)
        joined = collections.defaultdict(set)       # seen from either room
        for o, r in enumerate(self.rooms):
            for side in r["sides"].values():
                for _, kind, k in side:
                    if kind == "open_side" or (kind == "step" and k is not None and abs(self.rooms[k]["level"] - r["level"]) <= HEADROOM):
                        joined[o].add(k); joined[k].add(o)
        space_of = {}
        for ri in range(len(self.rooms)):
            if ri in space_of:
                continue
            rooms = flood([ri], lambda o: True, lambda o: joined[o])
            for o in rooms:
                space_of[o] = len(self.spaces)
                self.rooms[o]["space"] = len(self.spaces)
            top = max(self.rooms[o]["level"] for o in rooms)
            self.spaces.append(dict(rooms=sorted(rooms), level=min(self.rooms[o]["level"] for o in rooms),
                                    ceiling=min(top + self.room_height + 1, 25), overlooked_by=[]))
        # The floor columns of each level's rooms and connectors, and their space.
        upper = {}
        for ri, r in enumerate(self.rooms):
            x0, x1, z0, z1 = r["box"]
            for x in range(x0, x1 + 1):
                for z in range(z0, z1 + 1):
                    if self.columns[r["level"]].cls[(x, z)] in (FLOOR, FURNITURE):
                        upper[(r["level"], (x, z))] = r["space"]
        for c in self.connectors:
            if len(c["levels"]) == 1:
                sp = self.rooms[c["rooms"][0]]["space"]
                for level, comp in c["pieces"]:
                    for col in comp:
                        if level is not None:
                            upper[(level, col)] = sp
        upper_at = collections.defaultdict(list)
        for k, v in upper.items():
            upper_at[k[1]].append((k, v))
        base = [sp["ceiling"] for sp in self.spaces]
        for si, sp in enumerate(self.spaces):
            for ri in sp["rooms"]:
                level = self.rooms[ri]["level"]
                x0, x1, z0, z1 = self.rooms[ri]["box"]
                for x in range(x0, x1 + 1):
                    for z in range(z0, z1 + 1):
                        if self.columns[level].cls[(x, z)] != FLOOR:
                            continue
                        for n in plan_neighbors((x, z)):
                            for (ul, col), other in ((k, v) for k, v in upper_at.get(n, ()) if k[0] > level + HEADROOM):
                                if not self.g.solid[x, level + 1:ul + 2, z].any() and other not in sp["overlooked_by"]:
                                    sp["overlooked_by"].append(other)
        for sp in self.spaces:
            for other in sp["overlooked_by"]:
                sp["ceiling"] = max(sp["ceiling"], base[other])
        for r in self.rooms:
            r["ceiling"] = self.spaces[r["space"]]["ceiling"]
        for c in self.connectors:
            c["ceiling"] = max((self.rooms[ri]["ceiling"] for ri in c["rooms"]), default=None)

    def in_zone(self, c, level):
        x, z = c
        return any(x0 <= x <= x1 and z0 <= z <= z1 and y0 - 1 <= level <= y1 + 1 for (x0, x1), (y0, y1), (z0, z1) in self.zones)

    def _sides(self, r):
        """What lies past each column of the rectangle's four sides."""
        x0, x1, z0, z1 = r["box"]; level = r["level"]
        cols = self.columns[level]
        r["sides"] = {}
        for d, (dx, dz) in DIRS.items():
            if dx:
                along = [((x0 if dx < 0 else x1) + dx, z) for z in range(z0, z1 + 1)]
            else:
                along = [(x, (z0 if dz < 0 else z1) + dz) for x in range(x0, x1 + 1)]
            side = []
            for n in along:
                if not inside(*n):
                    kind, other = "open", None
                elif (level, n) in self.room_at:
                    kind, other = "open_side", self.room_at[(level, n)]
                elif (level, n) in self.connector_at or (None, n) in self.connector_at:
                    ci = self.connector_at.get((level, n), self.connector_at.get((None, n)))
                    kind, other = self.connectors[ci]["kind"], ci
                elif cols.cls[n] == WALL:
                    kind, other = "wall", cols.why.get(n)
                elif self.room_floor_levels.get(n):
                    kind, other = "step", next((self.room_at[(l, n)] for l in self.room_floor_levels[n] if (l, n) in self.room_at), None)
                else:
                    kind, other = "open", None
                side.append((n, kind, other))
            r["sides"][d] = side

    # -- output --------------------------------------------------------------------------------------------------

    def text(self):
        g = self.g
        lines = [f"grid {g.index} lib {g.lib} room height {self.room_height}: levels {self.levels}, {len(self.rooms)} rooms, "
                 f"{sum(c['kind'] != 'pocket' for c in self.connectors)} connectors, {len(self.flights)} flights",
                 "floors per height: " + " ".join(f"{y}:{n}" for y, n in sorted(self.floor_counts.items()))]
        for level in self.levels:
            for kind, a, b, end, cut in self.columns[level].extensions:
                lines.append(f"  level {level}: {kind} of wall {a}-{b} from {end} over {cut[0]}-{cut[-1]} ({len(cut)})")
        for ri, r in enumerate(self.rooms):
            x0, x1, z0, z1 = r["box"]
            lines.append(f"R{ri} space S{r['space']} level {r['level']} ceiling {r['ceiling']} x{x0}-{x1} z{z0}-{z1} ({x1 - x0 + 1}x{z1 - z0 + 1}) floors {r['floors']} furniture {r['furniture']}")
            for d, side in r["sides"].items():
                segs = []
                for (n, kind, other), group in _segments(side):
                    if kind in ("wall", "open") or other is None:
                        label = kind
                    else:
                        label = f"{kind}>{'R' if kind in ('open_side', 'step') else 'C'}{other}"
                    segs.append(f"{label}x{len(group)}")
                lines.append(f"   {d}: " + "  ".join(segs))
        for si, sp in enumerate(self.spaces):
            over = f" overlooked by {','.join(f'S{o}' for o in sp['overlooked_by'])} (double height)" if sp["overlooked_by"] else ""
            lines.append(f"S{si} level {sp['level']} ceiling {sp['ceiling']} rooms {','.join(f'R{r}' for r in sp['rooms'])}{over}")
        for ci, c in enumerate(self.connectors):
            if c["kind"] == "pocket":
                continue
            cols = [col for _, comp in c["pieces"] for col in comp]
            xs = [x for x, _ in cols]; zs = [z for _, z in cols]
            rooms = ",".join(f"R{r}" for r in c["rooms"])
            w = f" width {c['width']} ({'door' if c['width'] <= DOOR_WIDTH else 'arch'})" if c["kind"] == "passage" else ""
            at = "+".join(f"L{level}" if level is not None else "flight" for level, _ in c["pieces"])
            lines.append(f"C{ci} {c['kind']} {at} x{min(xs)}-{max(xs)} z{min(zs)}-{max(zs)} cols {len(cols)} rooms {rooms} ceiling {c['ceiling']}{w}")
        return "\n".join(lines)

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
            base = tuple(int(c * 255) for c in colorsys.hsv_to_rgb((ri * 0.618) % 1, 0.55, 0.75))
            x0, x1, z0, z1 = r["box"]
            cols = self.columns[r["level"]]
            for x in range(x0, x1 + 1):
                for z in range(z0, z1 + 1):
                    k = cols.cls[(x, z)]
                    col = base if k == FLOOR else tuple(c // 2 for c in base) if k == FURNITURE else (40, 0, 0)
                    dr.rectangle([x * s, z * s, x * s + s - 1, z * s + s - 1], fill=col)
        for level in self.levels:
            for x, z in self.columns[level].cuts:
                dr.rectangle([x * s + 1, z * s + 1, x * s + s - 2, z * s + s - 2], outline=(255, 255, 255))
        kinds = dict(passage=(60, 110, 255), stairs=(230, 60, 230), exit=(0, 60, 160), alcove=(60, 230, 80), pocket=(90, 90, 90))
        for c in self.connectors:
            for _, comp in c["pieces"]:
                for x, z in comp:
                    dr.rectangle([x * s + 2, z * s + 2, x * s + s - 3, z * s + s - 3], fill=kinds[c["kind"]])
        for f in self.flights:
            for x, y, z in f["cells"]:
                dr.text((x * s + 1, z * s), str(y), fill=(255, 255, 255))
        # Wall columns per level: white masonry, yellow cut, cyan invisible, magenta exterior, orange jamb.
        why = dict(masonry=(235, 235, 235), cut=(255, 220, 40), invisible=(40, 230, 255), exterior=(230, 60, 230), jamb=(255, 140, 30),
                   post=(120, 80, 40), against=(80, 120, 40))
        for level in self.levels:
            cols = self.columns[level]
            for c, k in cols.cls.items():
                if k == WALL or cols.why.get(c) in ("post", "against"):
                    x, z = c
                    dr.rectangle([x * s + 3, z * s + 3, x * s + s - 4, z * s + s - 4], fill=why[cols.why[c]])
        colors = dict(wall=(255, 255, 255), open_side=(60, 230, 80), passage=(60, 110, 255), stairs=(230, 60, 230), exit=(0, 60, 160),
                      alcove=(120, 255, 120), pocket=(90, 90, 90), step=(160, 60, 255), open=(255, 40, 40))
        for r in self.rooms:
            for d, side in r["sides"].items():
                dx, dz = DIRS[d]
                for (nx, nz), kind, _ in side:
                    x, z = nx - dx, nz - dz
                    if dx:
                        ex = x * s + (s - 1 if dx > 0 else 0)
                        seg = [ex, z * s, ex, z * s + s - 1]
                    else:
                        ez = z * s + (s - 1 if dz > 0 else 0)
                        seg = [x * s, ez, x * s + s - 1, ez]
                    dr.line(seg, fill=colors[kind], width=2)
        for ri, r in enumerate(self.rooms):
            x0, x1, z0, z1 = r["box"]
            dr.text(((x0 + x1) / 2 * s - 8, (z0 + z1) / 2 * s - 6), f"R{ri} L{r['level']}\nC{r['ceiling']}", fill=(0, 0, 0))
        dr.text((4, 64 * s + 4), f"grid {g.index} lib {g.lib}: {len(self.rooms)} rooms, levels {self.levels}", fill=(255, 255, 0))
        os.makedirs(out, exist_ok=True)
        path = os.path.join(out, f"plan_{g.index:03d}.png")
        img.save(path)
        open(os.path.join(out, f"plan_{g.index:03d}.txt"), "w").write(self.text() + "\n")
        return path


def learn_wall_bricks(lib, libs):
    """The library's wall bricks (see the module's comment)."""
    wall = collections.Counter(); furniture = collections.Counter(); floor = collections.Counter()
    for gi in range(S.GRM_START - S.GRID_START):
        if S.load_grid(gi)[0] != lib:
            continue
        g = S.Grid(gi, libs)
        p = Plan(g, None, zones_of(gi))
        for r in p.rooms:
            level = r["level"]; cols = p.columns[level]
            x0, x1, z0, z1 = r["box"]
            for x in range(x0, x1 + 1):
                for z in range(z0, z1 + 1):
                    k = cols.cls[(x, z)]
                    if k == FLOOR and g.brick[x, level, z]:
                        floor[int(g.brick[x, level, z])] += 1
                    elif k == FURNITURE and all(cols.cls.get(n) == FLOOR for n in plan_neighbors((x, z))):
                        furniture.update(int(g.brick[x, y, z]) for y in range(level + 1, min(25, level + 6)) if g.brick[x, y, z])
            for d, side in r["sides"].items():
                dx, dz = DIRS[d]
                for (nx, nz), kind, _ in side:
                    bx, bz = nx + dx, nz + dz
                    if kind != "wall" or (inside(bx, bz) and g.solid[bx, level + 1:level + 4, bz].any()):
                        continue
                    h, top = cols.run(nx, nz)
                    if top:
                        wall.update(int(g.brick[nx, y, nz]) for y in range(level + 1, level + 1 + h) if g.brick[nx, y, nz])
    return {b for b, n in wall.items() if n >= 2 and n >= 4 * (furniture[b] + floor[b])}


_WALL_BRICKS = {}


def wall_bricks(lib, libs):
    if lib not in _WALL_BRICKS:
        _WALL_BRICKS[lib] = learn_wall_bricks(lib, libs)
    return _WALL_BRICKS[lib]


def _segments(side):
    """Consecutive side columns of one kind and target."""
    out = []
    for item in side:
        if out and out[-1][0][1:] == item[1:]:
            out[-1][1].append(item)
        else:
            out.append((item, [item]))
    return out


if __name__ == "__main__":
    args = sys.argv[1:]
    out = args[args.index("-o") + 1] if "-o" in args else os.path.join(LOOKDEV, "plans")
    libs = {}
    for a in args:
        if a.isdigit():
            g = S.Grid(int(a), libs)
            p = Plan(g, wall_bricks(g.lib, libs), zones_of(g.index))
            print(p.text())
            print("  ->", p.plan(out))
