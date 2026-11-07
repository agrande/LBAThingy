"""Prototype of the LBA1 towns' building model (LBA1_ISLANDS_PLAN.md, "Phase 3 extended"): blocks, wall runs, openings,
roofs and stairs found from a scene's grid, drawn as a top-down plan to check against the iso render before any geometry.
Writes to Saved/LookDev/lba1_houses.

Usage: lba1_houses.py <command> [args]
  explore <scene> [...]   plan of the raw grid: heights, roof cells, every placed layout that isn't wall or ground (boxes,
                          labelled layout:size), to see what the model has to find
"""
import collections, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lba1
import lba1_town_survey as survey

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = os.path.join(PROJECT, "Saved/LookDev/lba1_houses")
PX = 14  # plan pixels per cell


class Scene:
    """A scene's cells [x, y, z]: brick, shape, layout (0-based), block; its library; per-brick classes, cached."""

    def __init__(self, n):
        self.n = n
        self.brick, self.shape, self.layout, self.block = survey.cells(n)
        self.lib = lba1.load_library(n)
        self._wall, self._thatch = {}, {}

    def is_wall(self, b):
        if b not in self._wall:
            self._wall[b] = bool(b) and survey.is_wall(b)
        return self._wall[b]

    def is_thatch(self, b):
        if b not in self._thatch:
            self._thatch[b] = bool(b) and survey.thatch(b)
        return self._thatch[b]

    def roof_layouts(self):
        """Layouts that are roof: at least 6 cells with 2 thatch bricks, or ramps with one (find_houses says why)."""
        if not hasattr(self, "_roofs"):
            def is_roof(L):
                bricks = [(sh, br) for sh, _, br in L["blocks"] if br]
                straw = sum(1 for _, br in bricks if self.is_thatch(br))
                ramps = any(2 <= sh <= 5 for sh, _ in bricks)
                return (np.prod(L["size"]) >= 6 and straw >= 2) or (ramps and straw >= 1)
            self._roofs = [li for li, L in enumerate(self.lib) if is_roof(L)]
        return self._roofs

    def anchor(self, x, y, z):
        """The placed layout a cell belongs to: (layout, its low corner x, y, z)."""
        li = int(self.layout[x, y, z]); b = int(self.block[x, y, z]); sx, sy, sz = self.lib[li]["size"]
        return (li, x - b % sx, y - (b // sx) % sy, z - b // (sx * sy))

    def instances(self):
        """Placed layouts: (layout, low corner x, y, z, size), each anchor found from any of its cells (a layout's block 0
        can be empty: an arch's doorway). Blocks run x fastest, then y, then z (FLBALayout::GetBlockCoords)."""
        anchors = set()
        for x, y, z in zip(*np.nonzero(self.brick > 0)):
            li = int(self.layout[x, y, z]); b = int(self.block[x, y, z]); sx, sy, sz = self.lib[li]["size"]
            anchors.add((li, int(x) - b % sx, int(y) - (b // sx) % sy, int(z) - b // (sx * sy), (sx, sy, sz)))
        return sorted(anchors)

    def top(self):
        """Per column [x, z]: the top filled cell's y (-1 empty)."""
        filled = self.brick > 0
        any_ = filled.any(axis=1)
        top = filled.shape[1] - 1 - np.argmax(filled[:, ::-1, :], axis=1)
        return np.where(any_, top, -1)


def explore(scenes):
    os.makedirs(OUT, exist_ok=True)
    for n in map(int, scenes):
        s = Scene(n)
        top = s.top()
        X, Z = top.shape
        img = Image.new("RGB", (X * PX, Z * PX), (30, 30, 36))
        d = ImageDraw.Draw(img)
        hmax = max(1, top.max())
        for x in range(X):
            for z in range(Z):
                y = top[x, z]
                if y < 0:
                    continue
                b = int(s.brick[x, y, z]); v = 60 + int(160 * y / hmax)
                col = (v, v, v)
                if s.is_thatch(b):
                    col = (220, 180, 40)
                elif s.shape[x, y, z] != 1:
                    col = (70, 110, 220)
                # plan: x right, z down (as the iso's left-down axis reads from above with north up = -z)
                d.rectangle([x * PX, z * PX, x * PX + PX - 1, z * PX + PX - 1], fill=col)
        wall_cols = np.zeros((X, Z), bool)
        for x, y, z in zip(*np.nonzero(s.brick > 0)):
            if s.lib[s.layout[x, y, z]]["size"][0] == 1 and s.lib[s.layout[x, y, z]]["size"][2] == 1 and s.is_wall(int(s.brick[x, y, z])):
                wall_cols[x, z] = True
        for x, z in zip(*np.nonzero(wall_cols)):
            d.rectangle([x * PX + 4, z * PX + 4, x * PX + PX - 5, z * PX + PX - 5], outline=(230, 60, 60))
        counts = collections.Counter()
        for li, x, y, z, (sx, sy, sz) in s.instances():
            L = s.lib[li]
            if sx == 1 and sz == 1:
                continue
            if sy == 1 and sx * sz >= 4:
                continue  # ground tiles
            counts[(li, (sx, sy, sz))] += 1
            d.rectangle([x * PX + 1, z * PX + 1, (x + sx) * PX - 2, (z + sz) * PX - 2], outline=(80, 230, 120))
            d.text((x * PX + 3, z * PX + 2), "%d" % li, fill=(80, 255, 120))
        path = os.path.join(OUT, "explore_%d.png" % n); img.save(path); print(path)
        for (li, size), c in sorted(counts.items(), key=lambda kv: -kv[1]):
            print("  %d:%d %s x%d" % (n, li, "x".join(map(str, size)), c))


SIDES = {"-x": (0, -1), "+x": (0, 1), "-z": (2, -1), "+z": (2, 1)}  # axis (0 x, 2 z), which end
SEEN = ("+x", "+z")  # the sides LBA1's iso camera drew (it looks from +x +z); the others were never drawn


class House:
    """A gable-roofed house: footprint [x0, x1) x [z0, z1) in cells, floor (first air row), eave and ridge (rows), the
    ridge's axis ('x' or 'z': the ridge runs along it), and per side its wall cells and openings."""

    def __init__(self, **kw):
        self.__dict__.update(kw)

    def __repr__(self):
        return "house x %d-%d z %d-%d floor %d eave %d ridge %d along %s" % (
            self.x0, self.x1, self.z0, self.z1, self.floor, self.eave, self.ridge, self.axis)


def components(mask):
    """8-connected components of a 2D bool mask [x, z], as lists of (x, z)."""
    seen = np.zeros_like(mask); out = []
    for sx, sz in zip(*np.nonzero(mask)):
        if seen[sx, sz]:
            continue
        stack = [(sx, sz)]; seen[sx, sz] = True; comp = []
        while stack:
            x, z = stack.pop(); comp.append((x, z))
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    a, b = x + dx, z + dz
                    if 0 <= a < mask.shape[0] and 0 <= b < mask.shape[1] and mask[a, b] and not seen[a, b]:
                        seen[a, b] = True; stack.append((a, b))
        out.append(comp)
    return out


def find_houses(s, min_cells=6):
    """Houses: each connected run of thatch columns is a roof; its bounding rectangle the footprint."""
    X, Y, Z = s.brick.shape
    # Roof pieces: layouts with thatch among their bricks (a slope's art straddles its cells: many of a roof's bricks
    # are less than 15 % straw, so a per-brick test leaves holes). Small props with straw (hay) are not roofs.
    # Old Burg (13) builds its roofs of small ramp layouts (1x2x1, 1x1x1); the ones facing away are slivers, too few
    # pixels to look like thatch, but their layout holds a thatch brick too (the ramp's step under it).
    roof_layouts = s.roof_layouts()
    thatch = np.isin(s.layout, roof_layouts) & (s.brick > 0)
    for b in np.unique(s.brick):
        if b and s.is_thatch(int(b)):
            thatch |= s.brick == b
    # Only roofs the camera saw: a roof cell with no brick over it (Proxim City has roof slices buried under a terrace).
    filled = s.brick > 0
    above = np.flip(np.cumsum(np.flip(filled, axis=1), axis=1), axis=1) - filled  # bricks over each cell
    thatch &= above == 0
    roof_cols = thatch.any(axis=1)
    # A roof's two slopes are apart by the ridge cap's column (its bricks aren't thatch colored): join across one cell.
    bridged = roof_cols.copy()
    bridged[1:-1, :] |= roof_cols[:-2, :] & roof_cols[2:, :]
    bridged[:, 1:-1] |= roof_cols[:, :-2] & roof_cols[:, 2:]
    houses = []
    for whole in components(bridged):
        if len(whole) < min_cells:
            continue
        for comp in split_ridges(s, thatch, whole):
            h = roof_house(s, thatch, filled, comp, min_cells)
            if h:
                houses.append(h)
    return houses


def roof_axis(s, thatch, comp):
    """The ridge's axis: roofs are solid cells with the slope drawn on them, so heights don't always tell. Where the roof
    is made of ramps (shapes 2-5 rise toward +x, +z, -z, -x) they do; slices one cell thin along the ridge (Proxim
    City's 3x4x1) too."""
    thin = collections.Counter()
    for x, z in comp:
        for y in np.nonzero(thatch[x, :, z])[0]:
            sh = int(s.shape[x, y, z])
            if sh in (2, 5):
                thin["z"] += 3
            elif sh in (3, 4):
                thin["x"] += 3
            else:
                sx, sy, sz = s.lib[int(s.layout[x, y, z])]["size"]
                thin["z" if sz < sx else "x" if sx < sz else "?"] += 1
    thin.pop("?", None)
    xs = [c[0] for c in comp]; zs = [c[1] for c in comp]
    return thin.most_common(1)[0][0] if thin else ("z" if max(zs) - min(zs) >= max(xs) - min(xs) else "x")


def split_ridges(s, thatch, comp):
    """Roofs side by side touch (Old Burg: 13's long house and the one behind it): split across the ridges at the valleys
    of the roof's height profile."""
    axis = roof_axis(s, thatch, comp)
    across = 0 if axis == "z" else 1  # the coordinate (x or z) that runs across the ridge
    prof = collections.defaultdict(lambda: -1)
    for x, z in comp:
        ys = np.nonzero(thatch[x, :, z])[0]
        if len(ys):
            k = (x, z)[across]; prof[k] = max(prof[k], int(ys.max()))
    ks = sorted(prof); p = [prof[k] for k in ks]
    # With ramps, each row across knows which way it rises (shapes 2, 3 rise toward +x, +z; 4, 5 toward -z, -x): a
    # house rises then falls, so the next one starts where a falling row is followed by a rising one.
    rise_cells = collections.Counter()
    for x, z in comp:
        d = cell_dir(s, thatch, x, z, across)
        if d:
            rise_cells[(x, z)[across], d] += 1
    # A boundary row: the first with rising cells after falling ones. Two houses offset along the ridge can share it
    # (Old Burg's 13 x 31: one's falling eave, the other's rising one), so its falling cells stay with the house before.
    ups = {k for k, sh in rise_cells if rise_cells[k, sh] > 0 and sh > 0}
    downs = {k for k, sh in rise_cells if rise_cells[k, sh] > 0 and sh < 0}
    bounds = []; fell = False
    for k in ks:
        if k in ups and fell:
            bounds.append(k); fell = False
            continue
        if k in downs:
            fell = True
    if not bounds:
        # No ramps to tell (slices): split at the valleys of the roof's height.
        for i in range(1, len(p) - 1):
            if p[i] < p[i - 1] and p[i] <= p[i + 1] and max(p[:i]) - p[i] >= 2 and max(p[i + 1:]) - p[i] >= 2:
                bounds.append(ks[i] + 1)
        if not bounds:
            return [comp]
    parts = [[] for _ in range(len(bounds) + 1)]
    for x, z in comp:
        k = (x, z)[across]
        part = sum(1 for b in bounds if k > b)
        if k in bounds and cell_dir(s, thatch, x, z, across) >= 0:
            part += 1
        parts[part].append((x, z))
    return [part for part in parts if part]


def cell_dir(s, thatch, x, z, across):
    """+1 if the column's roof ramps rise across the ridge's axis toward +, -1 if they fall, 0 if it has no ramps."""
    d = 0
    for y in np.nonzero(thatch[x, :, z])[0]:
        sh = int(s.shape[x, y, z])
        if across == 0 and sh in (2, 5):
            d += 1 if sh == 2 else -1
        elif across == 1 and sh in (3, 4):
            d += 1 if sh == 3 else -1
    return int(np.sign(d))


def roof_house(s, thatch, filled, comp, min_cells):
    X, Y, Z = s.brick.shape
    if len(comp) < min_cells:
        return None
    if True:
        xs = [c[0] for c in comp]; zs = [c[1] for c in comp]
        x0, x1, z0, z1 = min(xs), max(xs) + 1, min(zs), max(zs) + 1
        top = np.full((x1 - x0, z1 - z0), np.nan)
        for x, z in comp:
            ys = np.nonzero(thatch[x, :, z])[0]
            if len(ys):
                top[x - x0, z - z0] = ys.max()
        axis = roof_axis(s, thatch, comp)
        # Both slopes along the whole ridge: trim the rows along it to the longest run with thatch on both sides of the
        # ridge (Proxim City has a lone back slope buried behind a parapet, 42 x 19-21 z 0-10).
        rows = top if axis == "z" else top.T  # [across, along]
        mid = rows.shape[0] / 2
        both = [not np.isnan(rows[:int(mid), k]).all() and not np.isnan(rows[int(np.ceil(mid)):, k]).all() for k in range(rows.shape[1])]
        # The slope facing away is drawn as slivers on every third cell only (Old Burg): close gaps of up to 2.
        on = [k for k, b in enumerate(both) if b]
        for a, b in zip(on, on[1:]):
            if b - a <= 3:
                for k in range(a, b):
                    both[k] = True
        best, run, start = (0, 0), 0, 0
        for k, ok in enumerate(both + [False]):
            if ok:
                run += 1
                if run > best[1] - best[0]:
                    best = (k - run + 1, k + 1)
            else:
                run = 0
        if best[1] - best[0] < 2:
            return None
        if axis == "z":
            z0, z1 = z0 + best[0], z0 + best[1]
        else:
            x0, x1 = x0 + best[0], x0 + best[1]
        # The roof's pieces: the layouts that hold its thatch; the eave is their lowest row (a slice holds the wall under
        # the eave too: 42:189), the ridge over their highest.
        roof_layouts = {int(s.layout[x0 + x, y, z0 + z]) for x, y, z in zip(*np.nonzero(thatch[x0:x1, :, z0:z1]))}
        sub = np.isin(s.layout[x0:x1, :, z0:z1], list(roof_layouts)) & filled[x0:x1, :, z0:z1]
        ys = np.nonzero(thatch[x0:x1, :, z0:z1].any(axis=(0, 2)))[0]
        eave, ridge = int(ys.min()), int(ys.max()) + 1
        eave_pieces = int(np.nonzero(sub.any(axis=(0, 2)))[0].min())
        # Floor: down from under the roof's pieces through the hollow inside to the first brick; the most common over
        # the footprint's columns that have air there (walls don't).
        # The floor is the street's level just outside the walls the camera saw (+x, +z): inside, the hollow shell often
        # reaches down into the terrace it stands on (Proxim City's houses: 4 rows lower).
        floors = []
        outside = [(x1, z) for z in range(z0, z1)] + [(x, z1) for x in range(x0, x1)]
        for x, z in outside:
            if x >= X or z >= Z:
                continue
            solid = np.nonzero(filled[x, :eave_pieces, z])[0]
            if len(solid):
                floors.append(int(solid.max()) + 1)
        floor = collections.Counter(floors).most_common(1)[0][0] if floors else eave_pieces
        floor = min(floor, eave_pieces - 2)
        h = House(x0=x0, x1=x1, z0=z0, z1=z1, floor=floor, eave=eave_pieces, ridge=ridge, axis=axis, cells=len(comp))
        h.walls = {side: wall_side(s, h, side) for side in SIDES}
        return h


def wall_side(s, h, side):
    """A side's wall plane, the footprint's edge row of cells from the floor to the eave: per cell 'W' wall brick,
    '.' air, 'p' another piece; and its openings, runs of non-wall cells (air or pieces) across the plane."""
    axis, end = SIDES[side]
    if axis == 0:
        fixed = h.x1 - 1 if end > 0 else h.x0; along = range(h.z0, h.z1)
        cell = lambda a, y: (fixed, y, a)
    else:
        fixed = h.z1 - 1 if end > 0 else h.z0; along = range(h.x0, h.x1)
        cell = lambda a, y: (a, y, fixed)
    cols = list(along)
    # The side's base: the street's level just outside it (houses stand on slopes: Proxim City's harbour house has its
    # +z door up the steps, 3 rows over its +x side's street). The plane starts 3 rows lower, so a door down into a
    # pit (42's top-left house) is whole; below the street, any brick is wall.
    X, _, Z = s.brick.shape
    levels = []
    for a in cols:
        ox, _, oz = cell(a, 0)
        ox += end if axis == 0 else 0; oz += end if axis == 2 else 0
        if 0 <= ox < X and 0 <= oz < Z:
            solid = np.nonzero(s.brick[ox, :h.eave, oz] > 0)[0]
            if len(solid):
                levels.append(int(solid.max()) + 1)
    base = min(collections.Counter(levels).most_common(1)[0][0] if levels else h.floor, h.eave - 2)
    low = max(0, base - 3)
    plane = []
    for a in cols:
        col = ""
        for y in range(low, h.eave):
            x, yy, z = cell(a, y); b = int(s.brick[x, yy, z])
            col += "W" if b and (s.is_wall(b) or y < base) else "." if not b else "p"
        plane.append(col)
    # A single air cell inside wall is a blank in the wall's layout, not a hole.
    for k, col in enumerate(plane):
        plane[k] = "".join("W" if c == "." and 0 < y < len(col) - 1 and col[y - 1] == "W" and col[y + 1] == "W" else c
                           for y, c in enumerate(col))
    # Under the street, air is the hollow terrace (never drawn) unless it continues a hole from above (a door's pit).
    for k, col in enumerate(plane):
        col = list(col)
        for y in range(base - low - 1, -1, -1):
            if col[y] == "." and col[y + 1] != ".":
                col[y] = "W"
        plane[k] = "".join(col)
    walls = sum(c == "W" for col in plane for c in col)
    drawn = walls >= 0.25 * max(1, sum(len(col) for col in plane))
    openings = []
    if drawn:
        # Openings: connected non-wall cells (air or other pieces) inside the plane, below the eave. One that reaches the
        # floor is a door (air: open archway; a piece: a closed door), one over wall a window (or a plaque: a piece on
        # the wall). Air along the plane's ends is the wall being shorter there (a slope, a corner), not an opening.
        # Each placed piece is its own item (a plaque beside a door is not part of it), air holes another; a hole's frame
        # is the pieces around it (the arch over a doorway).
        H, W = len(plane[0]) if plane else 0, len(plane)
        label = {}
        for a in range(W):
            for y in range(H):
                if plane[a][y] == ".":
                    label[a, y] = "air"
                elif plane[a][y] == "p":
                    label[a, y] = s.anchor(*cell(cols[a], low + y))
        seen = set(); items = []
        for start in label:
            if start in seen:
                continue
            stack = [start]; seen.add(start); cells_ = []
            while stack:
                a, y = stack.pop(); cells_.append((a, y))
                for da, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    n = (a + da, y + dy)
                    if n in label and n not in seen and label[n] == label[start]:
                        seen.add(n); stack.append(n)
            items.append((label[start], cells_))
        holes = [(l, c) for l, c in items if l == "air"]
        for l, cells_ in holes:
            aa = [c[0] for c in cells_]; yy = [c[1] for c in cells_]
            if min(aa) == 0 or max(aa) == W - 1 or max(yy) == H - 1:
                continue  # the wall's outline (shorter at its ends, or under a slope), not a hole in it
            # Its frame: pieces touching the hole's box grown by a cell (the arch over it, the jambs beside it).
            frame = {label[k] for k in label if label[k] != "air" and min(aa) - 1 <= k[0] <= max(aa) + 1
                     and min(yy) - 1 <= k[1] <= max(yy) + 1}
            floor_row = base - low
            openings.append(dict(kind="door" if min(yy) <= floor_row else "window", at=cols[min(aa)],
                                 width=max(aa) - min(aa) + 1, bottom=low + min(yy) - h.floor, height=max(yy) - min(yy) + 1,
                                 pieces=sorted({p[0] for p in frame}), frame=frame, sunk=floor_row - min(yy)))
        framing = {p for o in openings for p in o["frame"]}
        for l, cells_ in items:
            if l == "air" or l in framing:
                continue
            aa = [c[0] for c in cells_]; yy = [c[1] for c in cells_]
            openings.append(dict(kind="closed door" if min(yy) <= base - low else "wall piece", at=cols[min(aa)],
                                 width=max(aa) - min(aa) + 1, bottom=low + min(yy) - h.floor, height=max(yy) - min(yy) + 1,
                                 pieces=[l[0]], frame={l}, sunk=0))
    return dict(fixed=fixed, along=(cols[0], cols[-1] + 1), base=base, plane=plane, drawn=drawn, openings=openings)


def find_openings(s, min_walls=6):
    """Openings in every wall face the camera saw (+x and +z), houses or not (the harbour's blocks, 12's fortress,
    Rebellion's ruins). In a plane of cells (a along it, y up), each cell is wall (a wall brick), a piece (another layout
    at least 2 rows tall: arches, doors, windows, plaques), air, or other solid (ground, caps, roofs, stairs). Air and
    pieces that can't be reached from the plane's border through air and pieces are enclosed by wall and solid: holes.
    Returns dicts: side, fixed, kind (door: air reaching the solid under it; window: air over wall; closed door / wall
    piece: a piece alone), along range, rows, the frame (pieces around the air)."""
    X, Y, Z = s.brick.shape
    out = []
    cls = np.zeros(s.brick.shape, np.uint8)  # 0 air, 1 wall, 2 piece, 3 other solid
    wall_b = {}
    roofs = set(s.roof_layouts())  # roof slices are tall pieces too, but they close a wall's top like any solid
    for x, y, z in zip(*np.nonzero(s.brick > 0)):
        b = int(s.brick[x, y, z])
        if b not in wall_b:
            wall_b[b] = s.is_wall(b)
        L = s.lib[int(s.layout[x, y, z])]["size"]
        if wall_b[b]:
            cls[x, y, z] = 1
        elif L[1] >= 2 and not (L[0] == 1 and L[2] == 1) and s.shape[x, y, z] == 1 and int(s.layout[x, y, z]) not in roofs:
            cls[x, y, z] = 2
        else:
            cls[x, y, z] = 3
    for side in ("+x", "+z"):
        n_planes = X if side == "+x" else Z
        for i in range(n_planes):
            plane = cls[i, :, :].T if side == "+x" else cls[:, :, i]  # [along, y]
            front = None
            if i + 1 < n_planes:
                front = cls[i + 1, :, :].T if side == "+x" else cls[:, :, i + 1]
            exposed = (plane == 1) & ((front == 0) if front is not None else True)
            if exposed.sum() < min_walls:
                continue
            A, H = plane.shape
            # Air with solid in front of it is under the street: a hollow terrace or shell (LBA1 drew neither), closed.
            hidden = (plane == 0) & (front != 0) if front is not None else np.zeros_like(plane, bool)
            open_ = ((plane == 0) & ~hidden) | (plane == 2)
            reach = np.zeros_like(open_)
            stack = [(a, y) for a in range(A) for y in range(H) if open_[a, y] and (a in (0, A - 1) or y in (0, H - 1))]
            for a, y in stack:
                reach[a, y] = True
            while stack:
                a, y = stack.pop()
                for da, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    b, z = a + da, y + dy
                    if 0 <= b < A and 0 <= z < H and open_[b, z] and not reach[b, z]:
                        reach[b, z] = True; stack.append((b, z))
            enclosed = open_ & ~reach
            # Items: air components, and each placed piece on its own.
            def cell(a, y):
                return (i, y, a) if side == "+x" else (a, y, i)
            label = {}
            for a, y in zip(*np.nonzero(enclosed)):
                label[a, y] = "air" if plane[a, y] == 0 else s.anchor(*cell(a, y))
            seen = set(); items = []
            for st in label:
                if st in seen:
                    continue
                stack = [st]; seen.add(st); cs = []
                while stack:
                    a, y = stack.pop(); cs.append((a, y))
                    for da, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        nb = (a + da, y + dy)
                        if nb in label and nb not in seen and label[nb] == label[st]:
                            seen.add(nb); stack.append(nb)
                items.append((label[st], cs))
            framing = set()
            for l, cs in items:
                if l != "air":
                    continue
                aa = [c[0] for c in cs]; yy = [c[1] for c in cs]
                # Seen from the front: the cells before the hole are open (or it's a recess the camera looked into).
                if front is not None and np.mean([front[a, y] in (0, 2) for a, y in cs]) < 0.5:
                    continue
                frame = {label[k] for k in label if label[k] != "air" and min(aa) - 1 <= k[0] <= max(aa) + 1
                         and min(yy) - 1 <= k[1] <= max(yy) + 1}
                # A real opening has a frame piece (an arch) or wall on both sides; else it's a gap between unrelated
                # things (under a fence, between a terrace and a house).
                rows_ = range(min(yy), max(yy) + 1)
                walled = np.mean([min(aa) > 0 and max(aa) + 1 < A and plane[min(aa) - 1, y] == 1 and plane[max(aa) + 1, y] == 1
                                  for y in rows_]) >= 0.5
                if not frame and not walled:
                    continue
                framing |= frame
                below = [plane[a, min(yy) - 1] for a in set(aa) if min(yy) > 0]
                kind = "door" if below and np.mean([c == 3 for c in below]) >= 0.5 else "window"
                out.append(dict(side=side, fixed=i, kind=kind, along=(min(aa), max(aa) + 1), rows=(min(yy), max(yy) + 1),
                                frame=sorted({p[0] for p in frame}), cells=len(cs)))
            for l, cs in items:
                if l == "air" or l in framing:
                    continue
                aa = [c[0] for c in cs]; yy = [c[1] for c in cs]
                if front is not None and np.mean([front[a, y] == 0 for a, y in cs]) < 0.5:
                    continue
                below = [plane[a, min(yy) - 1] for a in set(aa) if min(yy) > 0]
                kind = "closed door" if below and np.mean([c == 3 for c in below]) >= 0.5 else "wall piece"
                out.append(dict(side=side, fixed=i, kind=kind, along=(min(aa), max(aa) + 1), rows=(min(yy), max(yy) + 1),
                                frame=[l[0]], cells=len(cs)))
    return out


def openings_cmd(scenes):
    """Every opening of the scenes' drawn walls over the iso render (openings_<scene>.png): doors cyan, windows yellow,
    closed doors orange, wall pieces (plaques, round windows) white."""
    os.makedirs(OUT, exist_ok=True)
    colors = {"door": (0, 255, 255), "window": (255, 255, 0), "closed door": (255, 140, 0), "wall piece": (255, 255, 255)}
    for n in map(int, scenes):
        s = Scene(n)
        found = find_openings(s)
        iso_path = os.path.join(PROJECT, "Saved/LookDev/lba1_kit/iso_%d.png" % n)
        if not os.path.exists(iso_path):
            survey.iso([n])
        img = Image.open(iso_path).convert("RGB"); d = ImageDraw.Draw(img)
        kinds = collections.Counter()
        for o in found:
            kinds[o["kind"]] += 1
            a0, a1 = o["along"]; y0, y1 = o["rows"]; f = o["fixed"] + 1
            pts = ([(f, y0, a0), (f, y0, a1), (f, y1, a1), (f, y1, a0)] if o["side"] == "+x" else
                   [(a0, y0, f), (a1, y0, f), (a1, y1, f), (a0, y1, f)])
            d.line([iso_point(*p) for p in pts + pts[:1]], fill=colors[o["kind"]], width=3)
        path = os.path.join(OUT, "openings_%d.png" % n); img.save(path)
        print(path, dict(kinds))


def find_model(s):
    """The scene's building model: houses (with their walls' openings) and the openings of every other drawn wall."""
    found = find_houses(s)
    others = []
    for o in find_openings(s):
        a0, a1 = o["along"]
        inside = False
        for h in found:
            if o["side"] == "+x":
                inside |= h.x0 - 1 <= o["fixed"] < h.x1 + 1 and a0 < h.z1 and a1 > h.z0
            else:
                inside |= h.z0 - 1 <= o["fixed"] < h.z1 + 1 and a0 < h.x1 and a1 > h.x0
        if not inside:
            others.append(o)
    return found, others


KIND_COLORS = {"door": (0, 255, 255), "window": (255, 255, 0), "closed door": (255, 140, 0), "wall piece": (255, 255, 255)}


def draw_model(s, found, others, path):
    """The model over the scene's iso render: houses (floor cyan, eave magenta, ridge orange), openings by kind (door
    cyan, window yellow, closed door orange, wall piece white)."""
    iso_path = os.path.join(PROJECT, "Saved/LookDev/lba1_kit/iso_%d.png" % s.n)
    if not os.path.exists(iso_path):
        survey.iso([s.n])
    img = Image.open(iso_path).convert("RGB"); d = ImageDraw.Draw(img)

    def quad(side, f, a0, a1, y0, y1, color):
        pts = ([(f, y0, a0), (f, y0, a1), (f, y1, a1), (f, y1, a0)] if side == "+x" else
               [(a0, y0, f), (a1, y0, f), (a1, y1, f), (a0, y1, f)])
        d.line([iso_point(*p) for p in pts + pts[:1]], fill=color, width=4)

    for i, h in enumerate(found):
        box = [(h.x0, h.z0), (h.x1, h.z0), (h.x1, h.z1), (h.x0, h.z1)]
        for y, col in ((h.floor, (0, 200, 255)), (h.eave, (255, 0, 255))):
            d.line([iso_point(x, y, z) for x, z in box + box[:1]], fill=col, width=3)
        for x, z in box:
            d.line([iso_point(x, h.floor, z), iso_point(x, h.eave, z)], fill=(0, 200, 255), width=2)
        if h.axis == "x":
            zm = (h.z0 + h.z1) / 2; d.line([iso_point(h.x0, h.ridge, zm), iso_point(h.x1, h.ridge, zm)], fill=(255, 80, 0), width=5)
        else:
            xm = (h.x0 + h.x1) / 2; d.line([iso_point(xm, h.ridge, h.z0), iso_point(xm, h.ridge, h.z1)], fill=(255, 80, 0), width=5)
        for side, w in h.walls.items():
            axis, end = SIDES[side]
            for o in w["openings"]:
                y0 = h.floor + o["bottom"]
                quad("+x" if axis == 0 else "+z", w["fixed"] + (1 if end > 0 else 0), o["at"], o["at"] + o["width"],
                     y0, y0 + o["height"], KIND_COLORS[o["kind"]])
    for o in others:
        quad(o["side"], o["fixed"] + 1, o["along"][0], o["along"][1], o["rows"][0], o["rows"][1], KIND_COLORS[o["kind"]])
    img.save(path)


def model_cmd(scenes):
    os.makedirs(OUT, exist_ok=True)
    for n in map(int, scenes):
        s = Scene(n)
        found, others = find_model(s)
        kinds = collections.Counter(o["kind"] for h in found for w in h.walls.values() for o in w["openings"])
        kinds.update(o["kind"] for o in others)
        path = os.path.join(OUT, "model_%d.png" % n)
        draw_model(s, found, others, path)
        print("%s: %d houses, %s" % (path, len(found), dict(kinds)))


def iso_point(X, Y, Z):
    """Cell-unit point (x, y up, z) to lba1_town_survey.iso's pixels."""
    return (63 - Z + X) * 24 + 24, (X + Z) * 12 + (25 - Y) * 15


def houses(scenes):
    os.makedirs(OUT, exist_ok=True)
    for n in map(int, scenes):
        s = Scene(n)
        found = find_houses(s)
        iso_path = os.path.join(PROJECT, "Saved/LookDev/lba1_kit/iso_%d.png" % n)
        if not os.path.exists(iso_path):
            survey.iso([n])
        img = Image.open(iso_path).convert("RGB"); d = ImageDraw.Draw(img)
        for i, h in enumerate(found):
            print(i, h)
            box = [(h.x0, h.z0), (h.x1, h.z0), (h.x1, h.z1), (h.x0, h.z1)]
            for y, col in ((h.floor, (0, 255, 255)), (h.eave, (255, 0, 255))):
                d.line([iso_point(x, y, z) for x, z in box + box[:1]], fill=col, width=3)
            for x, z in box:
                d.line([iso_point(x, h.floor, z), iso_point(x, h.eave, z)], fill=(0, 255, 255), width=2)
            # ridge
            if h.axis == "x":
                zm = (h.z0 + h.z1) / 2; d.line([iso_point(h.x0, h.ridge, zm), iso_point(h.x1, h.ridge, zm)], fill=(255, 80, 0), width=4)
            else:
                xm = (h.x0 + h.x1) / 2; d.line([iso_point(xm, h.ridge, h.z0), iso_point(xm, h.ridge, h.z1)], fill=(255, 80, 0), width=4)
            for side, w in h.walls.items():
                print("   %s %s %s" % (side, "drawn" if w["drawn"] else "hidden", " ".join(w["plane"])))
                axis, end = SIDES[side]
                for o in w["openings"]:
                    print("      opening at %d width %d height %d pieces %s%s" % (o["at"], o["width"], o["height"], o["pieces"], " " + o["kind"]))
                    a0, a1 = o["at"], o["at"] + o["width"]; f = w["fixed"] + (1 if end > 0 else 0)
                    y0 = h.floor + o["bottom"]; y1 = y0 + o["height"]
                    pts = ([(f, y0, a0), (f, y0, a1), (f, y1, a1), (f, y1, a0)] if axis == 0 else
                           [(a0, y0, f), (a1, y0, f), (a1, y1, f), (a0, y1, f)])
                    d.line([iso_point(*p) for p in pts + pts[:1]], fill=(255, 255, 0), width=3)
            d.text(iso_point(h.x0, h.ridge + 1, h.z0), "H%d" % i, fill=(255, 255, 255))
        path = os.path.join(OUT, "houses_%d.png" % n); img.save(path); print(path)


def compose(lib, li):
    """A layout drawn as the iso camera saw it (RGBA)."""
    sx, sy, sz = lib[li]["size"]
    w = (sx + sz) * 24 + 48; h = (sx + sz) * 12 + sy * 15 + 40
    canvas = np.zeros((h, w, 4), np.uint8)
    for b, (shape, typ, br) in sorted(enumerate(lib[li]["blocks"]),
                                       key=lambda e: (e[0] % sx + e[0] // (sx * sy), (e[0] // sx) % sy, e[0] % sx)):
        if not br:
            continue
        x, y, z = b % sx, (b // sx) % sy, b // (sx * sy)
        img = lba1.brick(br); m = img[..., 3] > 0
        px, py = (sz - 1 - z + x) * 24, (x + z) * 12 + (sy - 1 - y) * 15
        canvas[py:py + 38, px:px + 48][m] = img[m]
    return Image.fromarray(canvas, "RGBA")


def layouts(n, ids):
    """A sheet of scene n's layouts (ids, or all placed ones that aren't walls or ground), labelled id size count."""
    os.makedirs(OUT, exist_ok=True)
    s = Scene(int(n))
    counts = collections.Counter((li, size) for li, x, y, z, size in s.instances())
    if ids:
        keys = [(int(i), tuple(s.lib[int(i)]["size"])) for i in ids]
    else:
        keys = [k for k, c in counts.most_common() if not (k[1][0] == 1 and k[1][2] == 1) and not (k[1][1] == 1 and k[1][0] * k[1][2] >= 4)]
    tiles = [(compose(s.lib, li), "%d %s x%d" % (li, "x".join(map(str, size)), counts[(li, size)])) for li, size in keys]
    cw = max(t.width for t, _ in tiles) + 8; ch = max(t.height for t, _ in tiles) + 20; cols = 8
    sheet = Image.new("RGB", (cw * cols, ch * ((len(tiles) + cols - 1) // cols)), (40, 40, 48))
    d = ImageDraw.Draw(sheet)
    for i, (t, label) in enumerate(tiles):
        at = (i % cols * cw, i // cols * ch)
        sheet.paste(t, (at[0] + 4, at[1] + 16), t); d.text((at[0] + 2, at[1] + 2), label, fill=(255, 255, 255))
    path = os.path.join(OUT, "layouts_%s.png" % n); sheet.save(path); print(path)


if __name__ == "__main__":
    command, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    {"explore": lambda: explore(args), "houses": lambda: houses(args), "openings": lambda: openings_cmd(args), "model": lambda: model_cmd(args), "layouts": lambda: layouts(args[0], args[1:])}.get(command, lambda: print(__doc__))()
