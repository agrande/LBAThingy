"""Proxima's rune stones as two road trips out of Proxim City (LBA1_ISLANDS_PLAN.md, Phase 5): a prototype of the sites'
shapes in the stub grammar and of the roads, before LBAPlanetStubs does it in C++.

Each site starts at its stone's LBA1 holomap marker (RESS.HQR entry 8, by scene: 45 the Eclipse Stone, 46 the Marked
Stone) and moves to the tip of painted land nearest to it. From the tip, a causeway runs out to a small round islet with
the stone, as in LBA1. The Marked Stone's site (north, the Hamalayi side) also gets a rock ridge across its arm with a
sheer face toward the city and a round hole at its foot; the road ends in front of it, and a footpath goes on through
the hole to the causeway. The roads leave Proxim City's pad and are found by a cheapest-path search over the ground,
then graded to a gentle slope and stamped in as earth with grass verges.

LBAPlanetSites (C++) follows this for the placement, land and roads, but the rock there is a mesh of its own with a real
bore through it (EmitRocks): this prototype's ridge with a slot is only a stand-in for judging placement from above.

Writes Saved/LookDev/rune_sites/<island>.png (left: the painting's land outline over the result; right: the ground)
and sites.json.
"""
import json, math, os, sys
import heapq
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stub_grammar as sg
import district_pads as dp
import lba1

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(sg.PROJECT, "Saved/LookDev/rune_sites")

# The sites: LBA1 scene of the stone (its holomap marker), and whether it is behind a cliff.
SITES = [
    dict(name="Marked Stone", scene=46, cliff=True),
    dict(name="Eclipse Stone", scene=45, cliff=False),
]
CITY = [42, 44]
TIP_INLAND = 3.0         # m: the tip is the land this far in from the coast nearest to the marker
CAUSEWAY_MIN = 14.0      # m from the tip to the islet's center (LBA1: 14 m to the Eclipse islet, 17 m past the cliff)
CAUSEWAY_MAX = 24.0
CAUSEWAY_HALF = 2.5      # m: a 3 m road and its verges
ISLET_RADIUS = 4.5       # m (LBA1: about 8 m across)
ROCK_LONG = 8.0          # m: the Marked Stone's rock, half its length along the way out (the road meets its face at the tip)
ROCK_WIDE = 7.0          # m: half its width
ROCK_HEIGHT = 6.5        # m over the ground (LBA1: 20 bricks, 6.4 m)
ROCK_EDGE = 1.2          # m over which its sides rise: sheer
LANDING = 3.5            # m: the low ground where the hole comes out, before the causeway
HOLE_HALF = 1.0          # m: the hole's half width (Twinsen walks through)
ROAD_HALF = 1.5          # m
VERGE = 1.0              # m of grass each side
SHOULDER = 2.5           # m over which the ground eases into the road
ROAD_GRADE = 0.12        # the steepest the road climbs (12 %)
STEP_GRADE_LIMIT = 0.45  # ground steeper than this between two cells is a cliff to the search


class Edits:
    """The sites' land and heights, for stub_grammar.grow."""

    def __init__(self, sites):
        self.sites = sites

    def land(self, mx, my, land):
        out = land.copy()
        for s in self.sites:
            out |= np.hypot(mx - s["islet"][0], my - s["islet"][1]) <= ISLET_RADIUS
            out |= segment_distance(mx, my, s["shore"], s["islet"]) <= CAUSEWAY_HALF
            if s["cliff"]:
                out |= rock_inside(mx, my, s) > 0
                out |= np.hypot(mx - s["shore"][0], my - s["shore"][1]) <= LANDING
                out |= segment_distance(mx, my, s["tip"], s["shore"]) <= CAUSEWAY_HALF
        return out

    def heights(self, mx, my, h, land):
        out = h.copy()
        for s in self.sites:
            # The causeway, its landing and the islet are shore: low and level.
            low = (segment_distance(mx, my, s["shore"], s["islet"]) <= CAUSEWAY_HALF) | (np.hypot(mx - s["islet"][0], my - s["islet"][1]) <= ISLET_RADIUS)
            if s["cliff"]:
                low |= np.hypot(mx - s["shore"][0], my - s["shore"][1]) <= LANDING
            out = np.where(low & land, np.minimum(out, sg.SHORE + 0.2), out)
            if not s["cliff"]:
                continue
            # The rock: sheer sides, a rolling top, a round hole at the foot of its face going straight through.
            inside = rock_inside(mx, my, s)
            rise = np.clip(inside / ROCK_EDGE, 0, 1)
            ground = sg.SHORE + 0.4
            top = ground + ROCK_HEIGHT * rise + 0.6 * sg.noise(h.shape, 5, 77) * rise
            d = s["dir"]; side = np.array([-d[1], d[0]])
            across = (mx - s["tip"][0]) * side[0] + (my - s["tip"][1]) * side[1]
            hole = (np.abs(across) <= HOLE_HALF) & (inside > -0.5)
            out = np.where(land & (inside > 0) & ~hole, np.maximum(out, top), out)
            out = np.where(land & hole, ground, out)
        return out


def rock_inside(mx, my, s):
    """How far inside the Marked Stone's rock a point is (m; negative outside): an ellipse from the tip outward, its
    outline wandering a little."""
    d = s["dir"]; side = np.array([-d[1], d[0]])
    c = s["tip"] + d * ROCK_LONG
    u = (mx - c[0]) * d[0] + (my - c[1]) * d[1]
    v = (mx - c[0]) * side[0] + (my - c[1]) * side[1]
    ang = np.arctan2(v / ROCK_WIDE, u / ROCK_LONG)
    wobble = 1.0 + 0.10 * np.sin(3 * ang + 1.3) + 0.06 * np.sin(5 * ang + 0.4) + 0.04 * np.sin(8 * ang + 2.1)
    r = np.hypot(u / ROCK_LONG, v / ROCK_WIDE) / wobble
    return (1.0 - r) * min(ROCK_LONG, ROCK_WIDE)


def segment_distance(mx, my, a, b):
    ab = np.asarray(b, float) - np.asarray(a, float)
    t = np.clip(((mx - a[0]) * ab[0] + (my - a[1]) * ab[1]) / max(ab @ ab, 1e-9), 0, 1)
    return np.hypot(mx - (a[0] + t * ab[0]), my - (a[1] + t * ab[1]))


def find_sites(g, mx, my, markers, city_at):
    """Each site's tip (the painted land nearest its marker, TIP_INLAND in), islet, and direction out."""
    # Only the island's main land (the city's): the painting has crumbs off its coasts.
    pieces, _ = ndimage.label(g["land"])
    cy, cx = int(round((city_at[1] - my[0, 0]) / sg.CELL)), int(round((city_at[0] - mx[0, 0]) / sg.CELL))
    main = pieces[cy, cx] if pieces[cy, cx] else np.bincount(pieces[pieces > 0]).argmax()
    land = pieces == main
    inland = ndimage.distance_transform_edt(land) * sg.CELL
    solid = land & (inland >= TIP_INLAND)
    pts = np.stack([mx[solid], my[solid]], -1)
    sites = []
    for spec in SITES:
        m = sg.map_location(*markers[spec["scene"]])
        tip = pts[np.argmin(np.hypot(*(pts - m).T))]
        out = m - tip
        reach = np.linalg.norm(out)
        d = out / reach if reach > 3 else (m - city_at) / np.linalg.norm(m - city_at)
        # The causeway starts at the tip, or past the rock where the hole comes out; the islet at least CAUSEWAY_MIN on.
        shore = tip + d * (2 * ROCK_LONG + LANDING - 1.0) if spec["cliff"] else tip
        islet = shore + d * np.clip(np.linalg.norm(m - shore), CAUSEWAY_MIN, CAUSEWAY_MAX)
        sites.append(dict(spec, marker=m, tip=tip, dir=d, shore=shore, islet=islet))
    return sites


def route(h, land, start_cells, goal, blocked):
    """Cheapest path over the 1 m grid (8 neighbours) from any start cell to the goal cell: length, weighted by grade."""
    ny, nx = h.shape
    INF = np.inf
    cost = np.full(h.shape, INF)
    prev = np.full(h.shape + (2,), -1, int)
    heap = []
    for y, x in start_cells:
        cost[y, x] = 0.0
        heapq.heappush(heap, (0.0, y, x))
    steps = [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
    gy, gx = goal
    while heap:
        c, y, x = heapq.heappop(heap)
        if c > cost[y, x]:
            continue
        if (y, x) == (gy, gx):
            break
        for dy, dx in steps:
            yy, xx = y + dy, x + dx
            if yy < 0 or xx < 0 or yy >= ny or xx >= nx or not land[yy, xx] or blocked[yy, xx]:
                continue
            run = math.hypot(dx, dy) * sg.CELL
            grade = abs(h[yy, xx] - h[y, x]) / run
            # A cliff can be crossed, at a price: the road cuts a ramp through it.
            step = run * (1.0 + 40.0 * min(grade, STEP_GRADE_LIMIT) ** 2 + 4.0 * grade) + (60.0 * abs(h[yy, xx] - h[y, x]) if grade > STEP_GRADE_LIMIT else 0.0)
            if c + step < cost[yy, xx]:
                cost[yy, xx] = c + step
                prev[yy, xx] = (y, x)
                heapq.heappush(heap, (c + step, yy, xx))
    path = [(gy, gx)]
    while tuple(prev[path[-1]]) != (-1, -1):
        path.append(tuple(prev[path[-1]]))
    return path[::-1], cost[gy, gx]


def smooth_path(points, passes=4):
    """Chaikin corner cutting, ends kept."""
    p = np.asarray(points, float)
    for _ in range(passes):
        q = [p[0]]
        for a, b in zip(p[:-1], p[1:]):
            q += [0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b]
        q.append(p[-1])
        p = np.array(q)
    return p


def grade_profile(z, spacing, grade):
    """Heights along a path, kept within `grade` of each other: the ground's, smoothed, then clamped both ways."""
    z = ndimage.gaussian_filter1d(z, 3.0 / spacing, mode="nearest")
    up = z.copy(); down = z.copy()
    for i in range(1, len(z)):
        up[i] = min(up[i], up[i - 1] + grade * spacing)
        up[i] = max(up[i], up[i - 1] - grade * spacing)
    for i in range(len(z) - 2, -1, -1):
        down[i] = min(down[i], down[i + 1] + grade * spacing)
        down[i] = max(down[i], down[i + 1] - grade * spacing)
    return 0.5 * (up + down)


def stamp_road(h, land, mx, my, path_m, spacing=0.5):
    """The road graded into the ground: level across, eased into the ground over SHOULDER. Returns h, road and verge masks."""
    seg = np.diff(path_m, axis=0); length = np.hypot(*seg.T)
    s = np.concatenate([[0], np.cumsum(length)])
    n = max(int(s[-1] / spacing), 2)
    t = np.linspace(0, s[-1], n)
    px = np.interp(t, s, path_m[:, 0]); py = np.interp(t, s, path_m[:, 1])
    x0, y0 = mx[0, 0], my[0, 0]
    cz = ndimage.map_coordinates(h, [(py - y0) / sg.CELL, (px - x0) / sg.CELL], order=1)
    road_z = grade_profile(cz, spacing, ROAD_GRADE)
    # Distance from each cell to the path and the road's height there (nearest sample).
    from scipy.spatial import cKDTree
    tree = cKDTree(np.stack([px, py], -1))
    dist, idx = tree.query(np.stack([mx.ravel(), my.ravel()], -1), distance_upper_bound=ROAD_HALF + VERGE + SHOULDER + 1)
    dist = dist.reshape(h.shape); idx = idx.reshape(h.shape)
    near = np.isfinite(dist)
    rz = np.zeros(h.shape); rz[near] = road_z[idx[near]]
    w = np.clip(1 - (dist - ROAD_HALF - VERGE) / SHOULDER, 0, 1); w = w * w * (3 - 2 * w)
    w[~near] = 0
    out = np.where(land, h * (1 - w) + rz * w, h)
    road = near & (dist <= ROAD_HALF) & land
    verge = near & (dist <= ROAD_HALF + VERGE) & land & ~road
    return out, road, verge, np.stack([px, py, road_z], -1)


def main():
    os.makedirs(OUT, exist_ok=True)
    markers = dp.holomap_markers()
    col, landtex, labels, extent, islands = sg.survey()
    isl = next(i for i in islands if i["name"] == "Proxima")
    wide = dict(isl)
    pad = 60.0
    wide["xs"] = (isl["xs"][0] - int(pad / sg.SURVEY), isl["xs"][1] + int(pad / sg.SURVEY))
    wide["ys"] = (isl["ys"][0] - int(pad / sg.SURVEY), isl["ys"][1] + int(pad / sg.SURVEY))
    mx, my = dp.island_grid(wide, extent, 0.0)
    g0 = sg.grow(wide, col, landtex, labels, extent)
    # Proxim City, fitted as the districts are.
    city = dp.District(dp.districts(CITY)[0], markers)
    score, turn, at, agree = dp.fit(city, (mx, my, g0["land"]), np.zeros(g0["land"].shape))
    city.place = (turn, at)
    sites = find_sites(g0, mx, my, markers, at)
    g = sg.grow(wide, col, landtex, labels, extent, Edits(sites))
    h, land, rgb, pad_land = dp.lay_pads(g, mx, my, [city])
    painted = g0["land"]

    # The roads: from the city's pad edge to the site's end (the tip, or in front of the cliff).
    cell = lambda p: (int(round((p[1] - my[0, 0]) / sg.CELL)), int(round((p[0] - mx[0, 0]) / sg.CELL)))
    edge = pad_land & ~ndimage.binary_erosion(pad_land, iterations=1)
    road_mask = np.zeros(h.shape, bool); verge_mask = np.zeros(h.shape, bool)
    report = []
    for s in sites:
        end = s["tip"] - s["dir"] * 2.0 if s["cliff"] else s["tip"]
        blocked = pad_land & ~edge
        if s["cliff"]:
            d = s["dir"]
            along = (mx - s["tip"][0]) * d[0] + (my - s["tip"][1]) * d[1]
            blocked |= along > -1.0          # nothing past the cliff's face: the road stops in front of it
        goal = cell(end)
        # Start from the city's edge cells nearest the site (the search picks among them).
        ey, ex = np.nonzero(edge)
        dd = np.hypot(mx[ey, ex] - end[0], my[ey, ex] - end[1])
        starts = [(ey[i], ex[i]) for i in np.argsort(dd)[:60]]
        path, c = route(h, land, starts, goal, blocked)
        pm = np.array([[mx[y, x], my[y, x]] for y, x in path])
        pm = smooth_path(pm)
        if not s["cliff"]:
            pm = np.vstack([pm, s["islet"]])   # on along the causeway to the islet
        h, road, verge, prof = stamp_road(h, land, mx, my, pm)
        road_mask |= road; verge_mask |= verge
        length = float(np.hypot(*np.diff(pm, axis=0).T).sum())
        dz = np.abs(np.diff(prof[:, 2])) / np.maximum(np.hypot(*np.diff(prof[:, :2], axis=0).T), 1e-6)
        from_city = float(np.linalg.norm(s["islet"] - at))
        report.append(dict(name=s["name"], marker=s["marker"].tolist(), tip=s["tip"].tolist(), islet=s["islet"].tolist(),
                           marker_to_islet=float(np.linalg.norm(s["marker"] - s["islet"])), city_to_islet=from_city,
                           road_m=length, steepest=float(dz.max()), climb=float(prof[:, 2].max() - prof[:, 2].min())))
        s["path"] = pm
        print(f"{s['name']}: islet {from_city:.0f} m from Proxim City ({np.linalg.norm(s['marker'] - s['islet']):.1f} m from its marker), "
              f"road {length:.0f} m, steepest {dz.max():.0%}, climbs {prof[:, 2].max() - prof[:, 2].min():.1f} m")
    json.dump(report, open(os.path.join(OUT, "sites.json"), "w"), indent=1)
    picture("Proxima", h, land, rgb, painted, road_mask, verge_mask, sites, mx, my, at)


def picture(name, h, land, rgb, painted, road, verge, sites, mx, my, city_at):
    gy, gx = np.gradient(h, sg.CELL)
    n = np.stack([-gx, -gy, np.ones_like(h)], -1); n /= np.linalg.norm(n, axis=-1, keepdims=True)
    light = np.array([-0.5, 0.5, 0.707]); light /= np.linalg.norm(light)
    shade = np.clip(n @ light, 0, 1) * 0.8 + 0.25
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    col = rgb.copy().astype(float)
    col[slope > 40] = sg.ROCK
    col[verge] = sg.GRASS
    col[road] = (150, 100, 60)
    img = np.where(land[..., None], col * shade[..., None], np.array([30, 60, 100]) * (1 + np.clip(h, -8, 0)[..., None] / 16))
    img = np.clip(img, 0, 255).astype(np.uint8)
    outline = painted & ~ndimage.binary_erosion(painted)
    left = img.copy(); left[outline] = (255, 255, 0)
    gap = np.full((land.shape[0], 8, 3), 10, np.uint8)
    im = Image.fromarray(np.concatenate([left, gap, img], 1)[::-1])
    d = ImageDraw.Draw(im)
    H = land.shape[0]
    def px(p, off=0):
        return (p[0] - mx[0, 0]) + off, H - 1 - (p[1] - my[0, 0])
    for s in sites:
        for p, c in ((s["marker"], (255, 60, 60)), (s["islet"], (255, 255, 255))):
            x, y = px(p); d.ellipse((x - 2, y - 2, x + 2, y + 2), outline=c)
        x, y = px(s["islet"], land.shape[1] + 8); d.text((x + 6, y - 6), s["name"], fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
    x, y = px(city_at, land.shape[1] + 8); d.text((x, y), "Proxim City", fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
    im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    im.save(os.path.join(OUT, name + ".png"))
    # Close-ups of the sites, 6 px per m.
    full = Image.fromarray(img[::-1])
    for s in sites:
        x, y = px(s["islet"] * 0.5 + s["tip"] * 0.5)
        c = full.crop((int(x) - 30, int(y) - 30, int(x) + 30, int(y) + 30)).resize((360, 360), Image.NEAREST)
        c.save(os.path.join(OUT, name + "_" + s["name"].replace(" ", "_") + ".png"))


if __name__ == "__main__":
    main()
