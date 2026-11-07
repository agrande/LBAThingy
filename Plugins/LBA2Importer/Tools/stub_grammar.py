"""Prototype of the stub islands' terrain grammar (L2), checked against LBA2's own islands (terrain_stats.py).

Usage: stub_grammar.py [out dir]   (default Saved/LookDev/stub_grammar)

Finds the holomap's unplaced islands like LBAPlanetStubs.cpp (HOLOMAP.HQR entry 2 painting, survey in 2 m cells to
latitude 14, islands over 500 m2 not under Citadel or the desert) and grows each one's ground by the grammar:

  1. Wanted height: rises with distance inland (to the island's own reach) and the painting's height hint, noise.
  2. Levels: a few flat benches (LBA2: 1 to 6 levels, 20-40 % of the land flat), quantised from the wanted height,
     each bench nested in the one below and kept a few meters back from it.
  3. Risers: a bench's edge rises over a width in meters: a cliff (1-2 m) mostly, a ramp (6-12 m) where noise says.
  4. Coast: a low shore bench at ~0.4 m; where the painting is sand the first bench sets back into a beach.
  5. Ground: rock on risers and the coast's rim, sand on beaches, grass/earth on benches by the painting, pale stone
     on the top bench where painted pale.

Writes <island>.png (heights | slopes | classes, like terrain_stats.py), grids.npz (heights m, classes, 1 m cells) for
the C++ port's check, and prints the same statistics as terrain_stats.py.
"""
import json, os, sys, zlib
import numpy as np
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr
import terrain_stats as ts

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/stub_grammar")
GAME = ts.GAME
RADIUS = 320.0          # m, the planet's
TEX = 256
SURVEY = 2.0            # m
LOWEST_LAT = 14.0
MIN_AREA = 500.0        # m2
CELL = 1.0              # m
MARGIN = 20.0           # m
PLACED = [(53.515551540287184, -141.17876198540827), (84.2, -9.6)]   # Citadel, desert (planet.json / preset)
NAMED = [("Principal", 43.3, 131.0), ("Proxima", 39.0, -101.6), ("Rebellion", 25.1, -21.1)]

# The grammar's numbers, from LBA2's islands (report.txt).
SHORE_MIN = 0.15        # m, the lowest land (sea level is 0)
SHORE = 0.4             # m, the low ground along the coast (Citadel shore median 0.86, desert 0.21)
PEAK_PER_ROOT = 0.15    # highest point / sqrt(land m2): Citadel 0.15, Otringal 0.28, desert 0.06
MIN_PEAK, MAX_PEAK = 4.0, 25.0
FIRST_BENCH = 2.8       # m, the first bench over the coast (Citadel 3.1, Otringal 1.9/3.1, Knartas 1.9)
COAST_RISE = 3.0        # m inland where the wanted height has left the shore
STEP_MIN, STEP_MAX = 2.5, 6.0     # m between benches (Citadel 6, 1.5; Otringal 1.2..7; Mosquibee 2, 2)
BENCH_SETBACK = 1.5     # m a bench keeps back from the one below
CLIFF_WIDTH = 1.4       # m a cliff riser takes (3 m up: 65 deg)
RAMP_WIDTH = 9.0        # m a ramp riser takes (3 m up: 18 deg)
RAMP_SHARE = 0.25       # of riser length (LBA2: 10-30 deg ~25 %, 45+ ~25 %)
EDGE_SMOOTHING = 1.5    # cells (gaussian) a bench's edge is smoothed over
COAST_SETBACK = 1.0     # m from the coast to the first bench on cliff coasts
BEACH_SETBACK = 10.0    # m on beaches
MIN_BENCH_AREA = 40.0   # m2
BAY_SIGMA = 8.0         # m: how wide a look at the coast decides bay or headland
BAY_MIN = 0.47          # land share around a coast cell above which it is a bay
ROLL = 2.0              # m of gentle rise and fall over benches (noise peak; typical slopes 5-10 deg)
EARTH_PATCHES = 0.3     # noise above this is bare earth among the grass (Citadel tops: 40 % earth, 30 % grass)
# Ground colors: LBA2's own (median of each class on Citadel/Otringal/desert, report.txt), sRGB.
ROCK = (136, 126, 128)
SAND = (220, 192, 120)
GRASS = (120, 140, 96)
EARTH = (124, 86, 58)
PALE = (206, 202, 192)


def painting():
    tex = np.frombuffer(hqr.entry(GAME + "HOLOMAP.HQR", 2)[:TEX * TEX], np.uint8).reshape(TEX, TEX)
    pal = np.frombuffer(hqr.entry(GAME + "RESS.HQR", 0)[:768], np.uint8).reshape(256, 3).astype(np.float32)
    col = pal[tex]
    land = ((col[..., 0] + col[..., 1] > 1.25 * col[..., 2] + 30) | (col.sum(-1) > 330)).astype(np.float32)
    return col, land


def sample(texels, mx, my):
    """LBAPlanetStubs' FPainting::Sample: equirect texture at map points (m), bilinear, wrapping in U."""
    lat = 90.0 - np.degrees(np.hypot(mx, my) / RADIUS)
    lon = np.mod(np.degrees(np.arctan2(my, mx)) + 360.0, 360.0)
    v = (lat + 90.0) / 180.0 * TEX - 0.5
    u = lon / 360.0 * TEX - 0.5
    v0 = np.floor(v).astype(int); u0 = np.floor(u).astype(int)
    fv = v - v0; fu = u - u0
    if texels.ndim == 3:
        fv = fv[..., None]; fu = fu[..., None]
    def at(r, c):
        return texels[np.clip(r, 0, TEX - 1), np.mod(c, TEX)]
    return (at(v0, u0) * (1 - fu) + at(v0, u0 + 1) * fu) * (1 - fv) + (at(v0 + 1, u0) * (1 - fu) + at(v0 + 1, u0 + 1) * fu) * fv


def blur(a, sigma):
    return ndimage.gaussian_filter(a, sigma, mode="nearest", truncate=3.0)


def map_location(lat, lon):
    r = RADIUS * np.radians(90.0 - lat)
    return np.array([np.cos(np.radians(lon)), np.sin(np.radians(lon))]) * r


def height_hint(c):
    """How high a painted color means (0..1) and its families (sand, green, brown, pale), read from the hue: the
    holomap paints dark (its own shading), so colors are judged against their own brightness. Olive green = lowland,
    brown = ridges, bright warm = sand, bright grey = bare stone up high."""
    lum = c.mean(-1)
    n = c / np.maximum(c.max(-1, keepdims=True), 1.0)
    r, g, b = n[..., 0], n[..., 1], n[..., 2]
    bright = np.clip((lum - 110) / 40, 0, 1)
    sat = 1 - n.min(-1)
    pale = bright * np.clip((0.3 - sat) / 0.15, 0, 1)
    sand = bright * np.clip((r - b - 0.2) / 0.15, 0, 1) * (1 - pale)
    green = np.clip((g - r + 0.05) / 0.1, 0, 1) * (1 - bright)
    brown = np.clip((r - g) / 0.12, 0, 1) * (1 - bright)
    rest = np.clip(1 - sand - green - brown - pale, 0, 1)
    tot = np.maximum(sand + green + brown + pale + rest, 1e-3)
    sand, green, brown, pale, rest = sand / tot, green / tot, brown / tot, pale / tot, rest / tot
    return 0.1 * sand + 0.35 * green + 0.8 * brown + 1.0 * pale + 0.5 * rest, sand, green, brown, pale


def noise(shape, scale, seed):
    """Smooth noise with features about `scale` cells wide, a third of it beyond +-1/3 (std 1/3), clamped to +-1."""
    rng = np.random.default_rng(seed)
    n = blur(rng.standard_normal(shape), scale / 2.5)
    return np.clip(n / max(3.0 * n.std(), 1e-6), -1, 1)


def survey():
    col, land = painting()
    extent = RADIUS * np.radians(90.0 - LOWEST_LAT)
    n = int(np.ceil(2 * extent / SURVEY))
    c = -extent + (np.arange(n) + 0.5) * SURVEY
    mx, my = np.meshgrid(c, c)
    s = blur(sample(land, mx, my), 1.5)
    inside = (s > 0.5) & (np.hypot(mx, my) < extent)
    labels, count = ndimage.label(inside)
    placed = set()
    for lat, lon in PLACED:
        p = map_location(lat, lon)
        cx = int((p[0] + extent) // SURVEY); cy = int((p[1] + extent) // SURVEY)
        placed |= set(np.unique(labels[cy - 2:cy + 3, cx - 2:cx + 3])) - {0}
    islands = []
    islets = 0
    # The C++ labels in scan order (row by row), as ndimage.label does.
    for lab in range(1, count + 1):
        cells = labels == lab
        if lab in placed or cells.sum() * SURVEY ** 2 < MIN_AREA:
            continue
        ys, xs = np.nonzero(cells)
        center = np.array([mx[cells].mean(), my[cells].mean()])
        name = None; best = 120.0
        for nm, lat, lon in NAMED:
            d = np.linalg.norm(map_location(lat, lon) - center)
            if d < best:
                best = d; name = nm
        if not name:
            islets += 1; name = f"Islet_{islets}"
        islands.append(dict(name=name, label=lab, xs=(xs.min(), xs.max()), ys=(ys.min(), ys.max()), center=center))
    return col, land, labels, extent, islands


def grow(isl, col, landtex, labels, extent, edits=None):
    """edits (optional): an object with land(mx, my, land) -> land and heights(mx, my, h, land) -> h, applied after the
    painting's land and after the benches (Tools/rune_sites.py's landmarks)."""
    seed = zlib.crc32(isl["name"].encode())
    x0 = -extent + isl["xs"][0] * SURVEY - MARGIN; y0 = -extent + isl["ys"][0] * SURVEY - MARGIN
    sx = int(np.ceil(((isl["xs"][1] - isl["xs"][0] + 1) * SURVEY + 2 * MARGIN) / CELL)) + 1
    sy = int(np.ceil(((isl["ys"][1] - isl["ys"][0] + 1) * SURVEY + 2 * MARGIN) / CELL)) + 1
    mx, my = np.meshgrid(x0 + np.arange(sx) * CELL, y0 + np.arange(sy) * CELL)
    soft = blur(sample(landtex, mx, my), 3.0)
    lx = np.floor((mx + extent) / SURVEY).astype(int); ly = np.floor((my + extent) / SURVEY).astype(int)
    n = labels.shape[0]
    ok = (lx >= 0) & (ly >= 0) & (lx < n) & (ly < n)
    own = np.zeros(mx.shape, np.float32)
    own[ok] = (labels[ly[ok], lx[ok]] == isl["label"])
    own = blur(own, 2.0)
    paint = np.stack([blur(sample(col[..., k], mx, my), 3.5) for k in range(3)], -1)
    land = soft * own > 0.5
    if edits:
        land = edits.land(mx, my, land)
    area = land.sum() * CELL ** 2
    hint, sand, green, brown, pale = (blur(a, 4.0) for a in height_hint(paint))
    inland = ndimage.distance_transform_edt(land) * CELL
    offshore = ndimage.distance_transform_edt(~land) * CELL

    # 1. Wanted height: up off the coast within COAST_RISE, then by the painting (normalised over the island: its
    # lowest colors low, its ridges high) and broad noise for hill masses, reaching full height half the reach in.
    reach = max(np.percentile(inland[land], 95), 5.0)
    peak = float(np.clip(PEAK_PER_ROOT * np.sqrt(area), MIN_PEAK, MAX_PEAK))
    lo_h, hi_h = np.percentile(hint[land], [10, 90])
    hn = np.clip((hint - lo_h) / (hi_h - lo_h + 0.1), 0, 1)
    mass = 0.5 + 0.5 * noise(land.shape, max(reach * 0.8, 10), seed)
    up = np.clip(0.55 * hn + 0.45 * mass, 0, 1) ** 1.3 * np.clip(inland / (0.5 * reach), 0, 1) ** 0.5
    first = FIRST_BENCH * (1 - sand)
    coast = np.clip(inland / COAST_RISE, 0, 1)
    wanted = coast * (first + (peak - first) * up) + 1.0 * noise(land.shape, 12, seed + 1)

    # 2. Levels: SHORE, the first bench, then STEP_MIN..STEP_MAX apart up to the wanted peak.
    rng = np.random.default_rng(seed + 2)
    levels = [SHORE, FIRST_BENCH]
    top = float(np.percentile(wanted[land], 99))
    while levels[-1] + STEP_MIN < top:
        levels.append(levels[-1] + float(rng.uniform(STEP_MIN, STEP_MAX)))
    levels[-1] = min(levels[-1], top + 0.5)

    # 4. Coast: beaches where the painting's coast is sand, else cliffs; the first bench keeps back that far.
    # Bays (more land than sea around the coast within ~BAY_SIGMA) take beaches, headlands cliffs: judged on the coast,
    # then carried inland (the nearby coast's average) so a beach runs from the water to its bench.
    coastline = (land & (inland <= 1.5 * CELL)).astype(np.float32)
    bay = blur(land.astype(np.float32), BAY_SIGMA)
    is_bay = np.clip((bay - BAY_MIN) / 0.1, 0, 1) * (0.6 + 0.4 * noise(land.shape, 30, seed + 3))
    is_bay = np.maximum(is_bay, (sand > 0.5).astype(np.float32))
    beachy = blur(coastline * is_bay, 6.0) / np.maximum(blur(coastline, 6.0), 1e-3) > 0.5
    setback = np.where(beachy, BEACH_SETBACK, COAST_SETBACK)
    benches = []
    below = land
    for i, lv in enumerate(levels[1:]):
        lo = levels[i]
        region = below & (wanted > (lo + lv) / 2)
        keep = ndimage.distance_transform_edt(below) * CELL > (setback if i == 0 else BENCH_SETBACK)
        region &= keep
        region = ndimage.binary_opening(region, iterations=2)
        lab, cnt = ndimage.label(region)
        if cnt:
            sizes = ndimage.sum(region, lab, range(1, cnt + 1)) * CELL ** 2
            region = np.isin(lab, 1 + np.nonzero(sizes >= MIN_BENCH_AREA)[0])
        if not region.any():
            break
        benches.append((lo, lv, region))
        below = region

    # 3. Risers: each bench's edge climbs over a cliff's or a ramp's width (blended where the noise turns), just inside
    # the bench; the signed distance smoothed so the edge runs between the cells instead of along them.
    ramp_noise = noise(land.shape, 25, seed + 4)
    ramp_from = np.quantile(ramp_noise, 1 - RAMP_SHARE)
    t = np.clip((ramp_noise - (ramp_from - 0.08)) / 0.16, 0, 1)
    width = CLIFF_WIDTH + (RAMP_WIDTH - CLIFF_WIDTH) * t * t * (3 - 2 * t)
    h = np.where(land, SHORE + 0.25 * np.clip(inland / 3, 0, 1) * np.where(beachy, 1.0, 0.0) - 0.1 * np.where(beachy, 1 - np.clip(inland / 3, 0, 1), 0), 0.0)
    for lo, lv, region in benches:
        sd = ndimage.distance_transform_edt(region) * CELL - ndimage.distance_transform_edt(~region) * CELL
        h = h + (lv - lo) * np.clip(blur(sd, EDGE_SMOOTHING) / width, 0, 1) * land
    # Rolling ground: benches aren't level floors (LBA2: a third of the land at 5-30 deg).
    h = np.maximum(h + ROLL * noise(land.shape, 14, seed + 6) * np.clip(inland / 4, 0, 1), SHORE_MIN) * land
    if edits:
        h = edits.heights(mx, my, h, land)
    # Under the sea: the shelf, as before.
    h = np.where(land, h, -np.minimum(8.0, 0.3 + 0.6 * offshore))

    # 5. Ground.
    gz, gx = np.gradient(h, CELL)
    slope = np.degrees(np.arctan(np.hypot(gx, gz)))
    cls = np.full(land.shape, 1, np.int8)
    tops = noise(land.shape, 12, seed + 5)
    cls[(green > brown) & (tops < EARTH_PATCHES)] = 1
    cls[(green <= brown) | (tops >= EARTH_PATCHES)] = 3
    cls[sand > 0.5] = 0
    if benches:
        cls[(pale > 0.4) & benches[-1][2]] = 4
    cls[beachy & (inland < BEACH_SETBACK)] = 0
    cls[(~beachy) & (inland < COAST_SETBACK + 1)] = 2
    cls[slope > 32] = 2
    cls[~land] = -1
    return dict(h=h, land=land, cls=cls, slope=slope, levels=levels, nbench=len(benches), peak=peak, area=area, paint=paint)


def stats(g):
    """terrain_stats' measures on a grown grid (1 m cells)."""
    L = g["land"]; h = g["h"]; slope = g["slope"]; cls = g["cls"]
    inland = ndimage.distance_transform_edt(L)
    flat = L & (slope < 6)
    shore = L & (inland <= 1.0)
    at3 = ndimage.maximum_filter(np.where(L & (inland > 2.5) & (inland <= 3.5), h, -1), size=7)
    hs = at3[shore]; hs = hs[hs >= 0]
    sh = [float(((slope[L] >= a) & (slope[L] < b)).mean()) for a, b in zip([0, 5, 10, 20, 30, 45, 60], [5, 10, 20, 30, 45, 60, 91])]
    return (f"{g['area']:.0f} m2, highest {h[L].max():.1f} m (peak {g['peak']:.1f}), {g['nbench']} benches "
            f"{' '.join(f'{l:.1f}' for l in g['levels'][:g['nbench'] + 1])}, flat {flat.mean() / L.mean():.0%}\n"
            f"   heights p10/25/50/75/90: {' '.join(f'{np.percentile(h[L], p):.1f}' for p in (10, 25, 50, 75, 90))}; "
            f"coast beach {(hs < 0.6).mean():.0%} cliff {(hs > 2).mean():.0%}\n"
            f"   slopes 0-5/5-10/10-20/20-30/30-45/45-60/60+: {' '.join(f'{v:.0%}' for v in sh)}\n"
            f"   classes {' '.join(f'{n} {(cls[L] == k).mean():.0%}' for k, n in enumerate(ts.CLASSES[:5]))}")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    ts.OUT = OUT
    col, landtex, labels, extent, islands = survey()
    grids = {}
    for isl in islands:
        g = grow(isl, col, landtex, labels, extent)
        print(f"== {isl['name']}: " + stats(g))
        ts.picture(isl["name"], g["h"], g["slope"], g["cls"], g["land"])
        grids[isl["name"] + "_h"] = g["h"]; grids[isl["name"] + "_cls"] = g["cls"]
    np.savez_compressed(os.path.join(OUT, "grids.npz"), **grids)
