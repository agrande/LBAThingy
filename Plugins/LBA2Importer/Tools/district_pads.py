"""Prototype of the stub islands' district pads (L3): LBA1's towns laid on the islands LBA2's holomap still paints.

Usage: district_pads.py [out dir]   (default Saved/LookDev/district_pads)

Each stub island (stub_grammar.py) gets LBA1's outdoor scenes of the same island. Scenes joined by edge exits make a
district (lba1_rosetta.lba1_frame puts them in one frame); a scene only reached by ferry or a path that isn't an edge
is a district of its own. Each district is placed:

  1. Where: the mean of its scenes' holomap markers (LBA1's RESS.HQR entry 8 holds one per scene, by scene number;
     on Citadel, where L1 knows the truth, each marker is 12-40 m off but their mean only 13 m).
  2. Which way: LBA1's x east and z north (as LBA2's islands' levels with heading 0; Citadel's LBA1 frame lies at
     -4 degrees on the map), unscaled (L1: one scene per LBA2 cube; districts moved, not scaled).
  3. Fit to the coast: the offset (within REACH) and turn (within TURN) where its land lies on the stub's land and
     its water on the stub's sea, the marker and heading 0 preferred (a gaussian prior); districts of one island
     placed largest first, each kept off the ones before.

Then each district's ground becomes a pad on the stub: its land at LBA1's ground heights (the streets' level among the
top bricks, over each scene's sea level), its water carved into sea, the stub's ground eased into the pad over BLEND. Land the stub's painting lacks under a pad is grown, as LBA2 grew the desert around the temple.

Writes <island>.png (stub | districts placed over the stub's land | stub with pads: heights, ground colors) and
pads.json (each district's scenes, placement, fit) and prints the fit.
"""
import json, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hqr import entry
import lba1
import lba1_rosetta as rosetta
import stub_grammar as sg

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/district_pads")
UNITS_PER_M = 800.0
LBA1_CELL = lba1.BRICK_XZ / UNITS_PER_M      # 0.64 m
LBA1_STEP = lba1.BRICK_Y / UNITS_PER_M       # 0.32 m

# LBA1's outdoor scenes per stub island (by LBA1 scene number; its interiors are left to L5).
OUTDOOR = {
    "Principal": [11, 12, 13, 17, 18, 19, 25, 24, 37],
    "Proxima": [42, 44],  # the rune stones (45-47) are sites, not towns: rune_sites.py, LBAPlanetSites
    "Rebellion": [59],
}
# The fit: how far from the markers' mean (m) and from heading 0 (degrees) a district may go, and how much it costs.
REACH = 45.0
TURN = 30.0
TURN_STEP = 3.0
PRIOR_REACH = 25.0     # m, the gaussian prior's sigma
PRIOR_TURN = 15.0      # degrees
OVERLAP_COST = 3.0     # per cell of a district over one placed before
# The pad.
GROUND_WINDOW = 17     # LBA1 cells (11 m) around a cell where its ground is looked for
GROUND_SHARE = 0.2     # the ground is the top bricks' height this share of that window is under
POND = 30.0            # m2: smaller pieces of water are ground
SEA_EASE = 8.0         # m (gaussian) over which scenes' sea levels ease into each other
BLEND = 6.0            # m over which the stub's ground eases into a pad
SEA_FLOOR = 1.2        # m under the sea where a pad's water is carved


def holomap_markers():
    """LBA1's holomap markers, one per scene: (latitude, longitude) on this planet's frame (L0's convention)."""
    d = entry(lba1.GAME + "RESS.HQR", 8)
    out = []
    for i in range(len(d) // 8):
        a, b = (v / 1024 * 2 * math.pi for v in struct.unpack_from("<hh", d, i * 8))
        ry = np.array([[math.cos(b), 0, math.sin(b)], [0, 1, 0], [-math.sin(b), 0, math.cos(b)]])
        rx = np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])
        v = rx @ ry @ np.array([0.0, 0.0, 1.0])
        out.append((math.degrees(-math.asin(v[1])), math.degrees(math.atan2(v[0], v[2])) + 270.0))
    return out


def edge_exit(d):
    """An exit across a scene's edge moves the hero a scene's width one way and little the other."""
    return max(abs(d[0]), abs(d[2])) >= 29000 and min(abs(d[0]), abs(d[2])) <= 8192


def districts(scenes):
    """Scenes grouped by edge exits, each group with its scenes' offsets in its first scene's frame."""
    parsed = {n: lba1.parse_scene(n) for n in scenes}
    left = list(scenes); out = []
    while left:
        first = left.pop(0)
        offsets = {first: np.zeros(3)}; todo = [first]
        while todo:
            a = todo.pop(0)
            for z in parsed[a]["zones"]:
                b = z["num"]
                if z["type"] != lba1.ZONE_CUBE or b not in left or b in offsets:
                    continue
                d = np.array(z["mins"], float) - np.array(z["info"][:3], float)
                if edge_exit(d):
                    offsets[b] = offsets[a] + d; todo.append(b)
        for b in offsets:
            if b in left:
                left.remove(b)
        out.append(offsets)
    return out


def east_north(lon):
    """The map's east and north (towards the pole) at a longitude: FLBAPlanetIsland::GetMapYaw's frame."""
    l = math.radians(lon)
    return np.array([-math.sin(l), math.cos(l)]), np.array([-math.cos(l), -math.sin(l)])


def composite(offsets):
    """rosetta.composite, plus the scene each cell shows."""
    cells = {n: (np.round(o[[0, 2]] / lba1.BRICK_XZ).astype(int), int(round(o[1] / lba1.BRICK_Y))) for n, o in offsets.items()}
    lo = np.min([c for c, _ in cells.values()], 0); hi = np.max([c for c, _ in cells.values()], 0) + 64
    shape = (hi[1] - lo[1], hi[0] - lo[0])
    height = np.full(shape, -999); rgb = np.zeros(shape + (3,)); land = np.zeros(shape, bool); scene = np.full(shape, -1)
    for n, (c, dy) in cells.items():
        h, col, water = lba1.plan(n)
        win = (slice(c[1] - lo[1], c[1] - lo[1] + 64), slice(c[0] - lo[0], c[0] - lo[0] + 64))
        hh = np.where(h >= 0, h + dy, -999)
        take = hh > height[win]
        height[win][take] = hh[take]; rgb[win][take] = col[take]; land[win][take] = ~water[take]
        scene[win][hh > -999] = np.where(take, n, scene[win])[hh > -999]
    return rgb, land, height, scene, lo * lba1.BRICK_XZ


class District:
    def __init__(self, offsets, markers):
        self.scenes = list(offsets)
        self.name = lba1.NAMES.get(self.scenes[0], f"scene {self.scenes[0]}")
        rgb, land, height, scene, origin = composite(offsets)
        self.rgb, self.land, self.height = rgb, land, height
        self.water = (height > -999) & ~land
        self.drawn = height > -999
        # Water in pieces under POND (fountains, gutters) and holes without bricks as small are ground, level with the
        # streets around them, not sea.
        def small(mask):
            pieces, count = ndimage.label(mask)
            sizes = ndimage.sum(mask, pieces, range(1, count + 1)) * LBA1_CELL ** 2
            return np.isin(pieces, 1 + np.nonzero(sizes < POND)[0])
        self.pond = small(self.water) | small(~self.drawn)
        self.water &= ~self.pond
        self.land = land | self.pond
        # The frame's pivot: the scenes' centers' mean (the markers' mean goes there), in m from cell 0, 0.
        centers = np.array([o[[0, 2]] + 16384 for o in offsets.values()])
        self.pivot = (centers.mean(0) - origin) / UNITS_PER_M
        lat, lon = np.mean([markers[n] for n in self.scenes], 0)
        lon = math.degrees(math.atan2(np.mean([math.sin(math.radians(markers[n][1])) for n in self.scenes]),
                                      np.mean([math.cos(math.radians(markers[n][1])) for n in self.scenes])))
        self.anchor = sg.map_location(lat, lon)
        self.east, self.north = east_north(lon)
        # The ground: the streets' level among the top bricks (their low GROUND_SHARE around each cell; houses and
        # trees stand above it), in m over the sea. LBA1 scenes keep their water at their own heights (Principal's at 0
        # and 3 cells in one frame): each scene's sea is its most common water height, or the district's for a scene
        # without water, eased across the scenes' seams.
        hi = np.where(self.drawn, height, 999).astype(float)
        low = np.minimum(ndimage.percentile_filter(hi, 100 * GROUND_SHARE, size=GROUND_WINDOW), hi)
        self.ground_y = np.minimum(ndimage.grey_closing(low, size=(3, 3)), hi)
        def sea_of(cells):
            ys, counts = np.unique(height[cells], return_counts=True)
            return float(ys[np.argmax(counts)])
        self.sea_y = sea_of(self.water) if self.water.any() else None
        sea = np.full(height.shape, self.sea_y or 0.0)
        for n in self.scenes:
            own = scene == n
            if (own & self.water).sum() > 64:
                sea[own] = sea_of(own & self.water)
        sea = sg.blur(sea, SEA_EASE / LBA1_CELL)
        self.ground_m = (self.ground_y - sea) * LBA1_STEP
        self.ground_rgb, nearest = self.ground_colors()
        self.ground_m[self.pond] = self.ground_m[nearest][self.pond]
        self.place = None

    def ground_colors(self):
        """Each land cell's ground color: its top brick's where that is the ground, else the nearest such cell's; and
        that cell."""
        on_ground = self.land & ~self.pond & (self.height - self.ground_y <= 2)
        if not on_ground.any():
            return self.rgb, np.indices(self.land.shape)
        _, (iz, ix) = ndimage.distance_transform_edt(~on_ground, return_indices=True)
        return self.rgb[iz, ix], (iz, ix)

    def area(self):
        return self.land.sum() * LBA1_CELL ** 2

    def frame(self, turn_deg):
        """District m (x along LBA1 x, y along LBA1 z, from the pivot) to map m: rotation columns."""
        t = math.radians(turn_deg)
        x = self.east * math.cos(t) + self.north * math.sin(t)
        y = -self.east * math.sin(t) + self.north * math.cos(t)
        return np.stack([x, y], 1)

    def to_local(self, mx, my, place=None):
        """Map points (m) to district cells (z, x), floats."""
        turn, at = place or self.place
        m = self.frame(turn)
        p = np.stack([mx - at[0], my - at[1]], -1) @ m  # m orthonormal: inverse = transpose
        return (p[..., 1] + self.pivot[1]) / LBA1_CELL, (p[..., 0] + self.pivot[0]) / LBA1_CELL

    def sample(self, grid, mx, my, place=None, fill=0):
        cz, cx = self.to_local(mx, my, place)
        iz, ix = np.floor(cz).astype(int), np.floor(cx).astype(int)
        ok = (iz >= 0) & (ix >= 0) & (iz < grid.shape[0]) & (ix < grid.shape[1])
        out = np.full(mx.shape + grid.shape[2:], fill, dtype=grid.dtype)
        out[ok] = grid[iz[ok], ix[ok]]
        return out


def fit(dist, grid, taken):
    """Best (turn, map location) for a district over an island's grid (stub land +1, sea -1; taken cells cost)."""
    mx, my, stub = grid
    cell = mx[0, 1] - mx[0, 0]
    score_map = np.where(stub, 1.0, -1.0)
    best = None
    ax0 = (dist.anchor[0] - mx[0, 0]) / cell; ay0 = (dist.anchor[1] - my[0, 0]) / cell
    for turn in np.arange(-TURN, TURN + 1e-6, TURN_STEP):
        # The district rasterised on the island's cells, its pivot at a kernel's center: land +1, water -1.
        r = int(math.ceil(np.hypot(*dist.land.shape) * LBA1_CELL / cell / 2 + np.hypot(*dist.pivot) / cell)) + 2
        ky, kx = np.mgrid[-r:r + 1, -r:r + 1] * cell
        weights = (dist.sample(dist.land.astype(np.int8), kx + dist.anchor[0], ky + dist.anchor[1], (turn, dist.anchor))
                   - dist.sample(dist.water.astype(np.int8), kx + dist.anchor[0], ky + dist.anchor[1], (turn, dist.anchor))).astype(float)
        corr = signal.fftconvolve(score_map, weights[::-1, ::-1], mode="same")
        if taken.any():
            corr -= OVERLAP_COST * signal.fftconvolve(taken, np.abs(weights)[::-1, ::-1], mode="same")
        oy, ox = np.mgrid[0:corr.shape[0], 0:corr.shape[1]]
        # fftconvolve "same" centers the kernel: corr[y, x] = the district pivoted at cell (y, x).
        dx, dy = (ox - ax0) * cell, (oy - ay0) * cell
        far = np.hypot(dx, dy)
        total = corr / max(np.abs(weights).sum(), 1) - 0.5 * (far / PRIOR_REACH) ** 2 * 0.2 - 0.5 * (turn / PRIOR_TURN) ** 2 * 0.2
        total[far > REACH] = -1e9
        i = np.unravel_index(np.argmax(total), total.shape)
        if best is None or total[i] > best[0]:
            best = (float(total[i]), float(turn), np.array([mx[0, 0] + i[1] * cell, my[0, 0] + i[0] * cell]),
                    float(corr[i] / max(np.abs(weights).sum(), 1)))
    return best


def island_grid(isl, extent, pad=60.0):
    """The island's 1 m grid (as stub_grammar.grow), widened by pad so districts may stick out to sea."""
    x0 = -extent + isl["xs"][0] * sg.SURVEY - sg.MARGIN - pad; y0 = -extent + isl["ys"][0] * sg.SURVEY - sg.MARGIN - pad
    sx = int(np.ceil(((isl["xs"][1] - isl["xs"][0] + 1) * sg.SURVEY + 2 * (sg.MARGIN + pad)) / sg.CELL)) + 1
    sy = int(np.ceil(((isl["ys"][1] - isl["ys"][0] + 1) * sg.SURVEY + 2 * (sg.MARGIN + pad)) / sg.CELL)) + 1
    return np.meshgrid(x0 + np.arange(sx) * sg.CELL, y0 + np.arange(sy) * sg.CELL)


def lay_pads(g, mx, my, dists):
    """The stub's ground with the districts' pads in it: heights (m), land, ground colors (sRGB), pad (0..1)."""
    stub_h, stub_land = g["h"], g["land"]
    colors = np.array([sg.SAND, sg.GRASS, sg.ROCK, sg.EARTH, sg.PALE], float)
    stub_rgb = colors[np.clip(g["cls"], 0, 4)]
    pad_land = np.zeros(stub_land.shape, bool); pad_water = np.zeros(stub_land.shape, bool)
    pad_h = np.zeros(stub_h.shape); pad_rgb = np.zeros(stub_rgb.shape)
    for d in dists:
        on = d.sample(d.land.astype(np.int8), mx, my).astype(bool) & ~pad_land
        wet = d.sample(d.water.astype(np.int8), mx, my).astype(bool) & ~pad_land & ~on
        h = d.sample(d.ground_m, mx, my, fill=0.0)
        if d.sea_y is None:
            # No water to stand over: its median ground where the stub's is.
            h = h - np.median(h[on]) + np.median(stub_h[on])
        pad_h[on] = np.maximum(h[on], sg.SHORE_MIN)
        pad_rgb[on] = d.sample(d.ground_rgb, mx, my)[on]
        pad_land |= on; pad_water |= wet
    pad = pad_land | pad_water
    # Off a pad, the stub's ground eases from the pad's nearest height to its own over BLEND.
    off, (iy, ix) = ndimage.distance_transform_edt(~pad_land, return_indices=True)
    off = off * sg.CELL
    t = np.clip(off / BLEND, 0, 1); t = t * t * (3 - 2 * t)
    land = (stub_land & ~pad_water) | pad_land
    h = np.where(pad_land, pad_h, pad_h[iy, ix] * (1 - t) + stub_h * t)
    rgb = np.where(pad_land[..., None], pad_rgb, stub_rgb)
    offshore = ndimage.distance_transform_edt(~land) * sg.CELL
    sea_floor = -np.minimum(8.0, 0.3 + 0.6 * offshore)
    h = np.where(land, np.maximum(h, sg.SHORE_MIN), np.minimum(sea_floor, np.where(pad_water, -SEA_FLOOR, 0)))
    return h, land, rgb, pad_land


def main():
    os.makedirs(OUT, exist_ok=True)
    markers = holomap_markers()
    col, landtex, labels, extent, islands = sg.survey()
    report = {}
    for isl in islands:
        if isl["name"] not in OUTDOOR:
            continue
        # The stub as L2 grows it, on a wider grid.
        isl_wide = dict(isl)
        pad = 60.0
        isl_wide["xs"] = (isl["xs"][0] - int(pad / sg.SURVEY), isl["xs"][1] + int(pad / sg.SURVEY))
        isl_wide["ys"] = (isl["ys"][0] - int(pad / sg.SURVEY), isl["ys"][1] + int(pad / sg.SURVEY))
        g = sg.grow(isl_wide, col, landtex, labels, extent)
        mx, my = island_grid(isl_wide, extent, 0.0)
        stub = g["land"]
        dists = [District(o, markers) for o in districts(OUTDOOR[isl["name"]])]
        dists.sort(key=lambda d: -d.area())
        taken = np.zeros(stub.shape)
        print(f"== {isl['name']}: stub {g['area']:.0f} m2, {len(dists)} districts")
        report[isl["name"]] = []
        for d in dists:
            score, turn, at, agree = fit(d, (mx, my, stub), taken)
            d.place = (turn, at)
            on = d.sample(d.land.astype(np.int8), mx, my).astype(bool)
            wet = d.sample(d.water.astype(np.int8), mx, my).astype(bool)
            taken += on | wet
            moved = np.linalg.norm(at - d.anchor)
            print(f"   {d.name:28s} scenes {d.scenes}: land {d.area():.0f} m2, sea level y {d.sea_y}, turn {turn:+.0f} deg, "
                  f"{moved:.1f} m from the markers, agreement {agree:+.2f}; its land on stub land {(on & stub).sum() / max(on.sum(), 1):.0%}, "
                  f"its water on stub sea {(wet & ~stub).sum() / max(wet.sum(), 1):.0%}")
            report[isl["name"]].append(dict(name=d.name, scenes=d.scenes, turn_deg=turn, map_m=at.tolist(),
                                            anchor_m=d.anchor.tolist(), agreement=agree, land_m2=d.area()))
        picture(isl["name"], g, mx, my, dists)
    json.dump(report, open(os.path.join(OUT, "pads.json"), "w"), indent=1)


def picture(name, g, mx, my, dists):
    land = g["land"]
    base = np.zeros(land.shape + (3,), np.uint8); base[...] = (30, 60, 100)
    hn = np.clip(g["h"] / max(g["h"].max(), 1), 0, 1)
    base[land] = (np.stack([hn] * 3, -1)[land] * 180 + 50).astype(np.uint8)
    over = base.copy().astype(float)
    for d in dists:
        drawn = d.sample(d.drawn.astype(np.int8), mx, my).astype(bool)
        rgb = d.sample(d.rgb, mx, my)
        over[drawn] = 0.25 * over[drawn] + 0.75 * rgb[drawn]
    # The stub with its pads: ground colors, shaded by a light from the north-west, 45 degrees up.
    h, new_land, rgb, _ = lay_pads(g, mx, my, dists)
    gy, gx = np.gradient(h, sg.CELL)
    n = np.stack([-gx, -gy, np.ones_like(h)], -1); n /= np.linalg.norm(n, axis=-1, keepdims=True)
    light = np.array([-0.5, 0.5, 0.707]); light /= np.linalg.norm(light)
    shade = np.clip(n @ light, 0, 1) * 0.8 + 0.25
    after = np.where(new_land[..., None], rgb * shade[..., None], np.array([30, 60, 100]) * (1 + h[..., None] / 16))
    gap = np.full((land.shape[0], 8, 3), 10, np.uint8)
    img = Image.fromarray(np.concatenate([base, gap, over.astype(np.uint8), gap, np.clip(after, 0, 255).astype(np.uint8)], 1)[::-1])
    dr = ImageDraw.Draw(img)
    for d in dists:
        for p, c in ((d.anchor, (255, 60, 60)), (d.place[1], (255, 230, 40))):
            x = (p[0] - mx[0, 0]) + land.shape[1] + 8; y = land.shape[0] - 1 - (p[1] - my[0, 0])
            dr.ellipse((x - 3, y - 3, x + 3, y + 3), fill=c)
        x = (d.place[1][0] - mx[0, 0]) + land.shape[1] + 8; y = land.shape[0] - 1 - (d.place[1][1] - my[0, 0])
        dr.text((x + 5, y - 5), d.name, fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(os.path.join(OUT, name + ".png"))


if __name__ == "__main__":
    main()
