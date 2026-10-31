"""Prototype of the Hamalayi belt as the holomap has it (LBAPlanetBelt.cpp's port follows it).

Usage: belt_grammar.py [out dir]   (default Saved/LookDev/belt_grammar)

The belt grows on a grid of longitude columns by latitude rows (2 m across, ~2.3 m along at its middle), from:

  1. Footprint: the holomap's painting (HOLOMAP.HQR entry 2, RESS palette 0). Land = its blues, whites, browns and
     greens; water = its dark sea and the teal fringe, which is shallow (its speckles' density says how shallow). Only
     the ring of land around the planet is kept (not the islands of either hemisphere).
  2. Wanted height: the holomap's own (HOLOGLOB.CPP: radius 1000 + 2 x HMT altitude, HMT = HOLOMAP.HQR entry 1, 17
     latitudes x 32 longitudes), bilinear between its vertices, times what the painting's brightness says (dark blue
     low, white high: about the same mean); where the holomap's mesh is at 0 but the painting has land, a low ground
     from the painting alone. Small noise under a texel's size.
  3. Benches, as the stub islands' grammar (stub_grammar.py) but taller: a low shore, a first bench, a wall (its riser
     always a cliff, and every column's crest above it, so the islands' hemisphere is closed to walkers), then levels
     STEP_MIN..STEP_MAX apart up to the peaks, each nested in the one below; risers cliffs, a share of them ramps.
  4. Ground: the painting's colors through the belt's calibrated albedos (dark ice, light ice, snow, brown rock),
     cliff ice on steep ground, pale shelf under shallow water.

Writes sheet_*.png (painting | grown colors | hillshade), heights.npz, and prints statistics: heights against the
holomap's, slopes, whether anyone can walk from the islands' side to the far side.
"""
import math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/belt_grammar")
GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2").rstrip("/\\") + "/"
LBA1 = os.environ.get("LBA1_GAME", "C:/GOG Games/tlba-classic/Common").rstrip("/\\") + "/"
RADIUS = 320.0          # m
TEX = 256
HOLO_RADIUS = 1000.0    # HOLOGLOB.CPP TabRayon[Twinsun]; vertex = radius + 2 * altitude

COLUMNS = 1536
ROW_M = 2.0             # m across
LAT_TOP, LAT_BOTTOM = 18.0, -46.0   # the grid's rows (islands' side first)
LAND_LAT = 14.0         # land nearer the pole is the stub islands' (LBAPlanetStubs' survey)
COAST_WANDER = 0.35     # of the painting's land share: how far coasts wander off its texels
MIN_PIECE = 300.0       # m2: smaller pieces of land are dropped

# Heights (m).
SHORE = 0.4
FIRST_BENCH = 2.5
WALL = 10.0             # the first bench over the wall: its riser is a cliff everywhere
STEP_MIN, STEP_MAX = 3.5, 7.0
BRIGHT_LOW, BRIGHT_HIGH = 0.35, 1.3   # wanted height = holomap's x this, from dark to white
OFF_MESH = 8.0          # wanted height of white painted land where the holomap's mesh is at 0
CREST_WIDTH = 12.0      # m of each column's crest held over the wall
BENCH_SETBACK = 2.0
COAST_SETBACK = 2.0
MIN_BENCH_AREA = 60.0
CLIFF_WIDTH = 2.0
RAMP_WIDTH = 12.0
RAMP_SHARE = 0.25
EDGE_SMOOTHING = 2.0    # cells
COAST_RISE = 25.0       # m inland over which the wanted height leaves the shore (a flank before the wall)
LEVEL_DRIFT = 2.5       # m the wall and the levels over it drift up and down along the belt (features ~100 m)
DETAIL = 2.5            # m of noise on the wanted height (features ~10 m)
ROLL = 0.6              # m of rolling ground on benches
SHELF_DEPTH = 2.5       # m at the shelf's edge (speckled water), deep water falls to SKIRT
SKIRT = 8.0
WALK_LIMIT = 55.0       # degrees Twinsen walks

# Albedos (sRGB), calibrated for the enhanced grade on the old belt (lba2-globe-brainstorm: belt colors).
DARK_ICE = np.array([86, 122, 146.])
LIGHT_ICE = np.array([170, 190, 200.])
SNOW_SHADE = np.array([196, 216, 230.])
SNOW = np.array([232, 238, 246.])
BROWN = np.array([131, 103, 55.])
BROWN_CLIFF = np.array([96, 78, 50.])
GREEN = np.array([110, 124, 84.])
SHELF = np.array([176, 222, 204.])
DEEP = np.array([40, 70, 90.])
FROZEN = np.array([150, 186, 204.])
CAP_SOFTNESS = 4.0      # m over which the cap eases in
CALIBRATION_PASSES = 2  # the wanted height is scaled per 11.25 degrees until the built mean meets the holomap's


def painting():
    tex = np.frombuffer(hqr.entry(GAME + "HOLOMAP.HQR", 2)[:TEX * TEX], np.uint8).reshape(TEX, TEX)
    pal = np.frombuffer(hqr.entry(GAME + "RESS.HQR", 0)[:768], np.uint8).reshape(256, 3).astype(np.float32)
    return pal[tex]


def classify(c):
    """Per texel: land (0/1), brightness (0 dark blue .. 1 white), brown, green, and the shallow fringe (teal water)."""
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    s = r + g + b
    teal = (r < 80) & (g > 0.85 * b) & (s > 150)
    water = (s < 150) | teal
    brown = (r > b + 10) & ~water
    green = (g > r) & (g > b) & ~water & ~brown
    bright = np.clip((s - 237) / (725 - 237), 0, 1)
    return (~water).astype(np.float32), bright, brown.astype(np.float32), green.astype(np.float32), teal.astype(np.float32)


def blur(a, sigma):
    """Gaussian over the grid: rows clamp, columns wrap around the planet."""
    return ndimage.gaussian_filter(a, sigma, mode=("nearest", "wrap"), truncate=3.0)


def edt(mask, sampling):
    """Distance (m) to the nearest cell outside mask, columns wrapping."""
    pad = 96
    m = np.concatenate([mask[:, -pad:], mask, mask[:, :pad]], 1)
    return ndimage.distance_transform_edt(m, sampling=sampling)[:, pad:-pad]


def noise(shape, scale, seed, sampling):
    rng = np.random.default_rng(seed)
    n = blur(rng.standard_normal(shape), (scale / sampling[0] / 2.5, scale / sampling[1] / 2.5))
    return np.clip(n / max(3.0 * n.std(), 1e-6), -1, 1)


def grid():
    nrows = int(math.ceil((LAT_TOP - LAT_BOTTOM) / math.degrees(ROW_M / RADIUS)))
    lat = LAT_TOP - (np.arange(nrows) + 0.5) * math.degrees(ROW_M / RADIUS)
    lon = (np.arange(COLUMNS) + 0.5) * 360.0 / COLUMNS
    return np.meshgrid(lat, lon, indexing="ij")


def sample(texels, lat, lon):
    """Equirect texture at (lat, lon), bilinear, wrapping in U (LBAPlanetStubs' FPainting::Sample)."""
    v = (lat + 90.0) / 180.0 * TEX - 0.5
    u = lon / 360.0 * TEX - 0.5
    v0 = np.floor(v).astype(int); u0 = np.floor(u).astype(int)
    fv = v - v0; fu = u - u0
    if texels.ndim == 3:
        fv = fv[..., None]; fu = fu[..., None]
    at = lambda rr, cc: texels[np.clip(rr, 0, TEX - 1), np.mod(cc, TEX)]
    return (at(v0, u0) * (1 - fu) + at(v0, u0 + 1) * fu) * (1 - fv) + (at(v0 + 1, u0) * (1 - fu) + at(v0 + 1, u0 + 1) * fu) * fv


def holomap_heights(lat, lon):
    """The holomap's mesh over the sea (m on our planet), bilinear between its 11.25-degree vertices."""
    alt = np.frombuffer(hqr.entry(GAME + "HOLOMAP.HQR", 1)[:17 * 32], np.uint8).reshape(17, 32).astype(np.float32)
    h = 2.0 * alt / HOLO_RADIUS * RADIUS
    v = (lat + 90.0) / 11.25; u = lon / 11.25
    v0 = np.clip(np.floor(v).astype(int), 0, 15); u0 = np.floor(u).astype(int)
    fv = v - v0; fu = u - u0
    at = lambda rr, cc: h[rr, np.mod(cc, 32)]
    mesh = (at(v0, u0) * (1 - fu) + at(v0, u0 + 1) * fu) * (1 - fv) + (at(v0 + 1, u0) * (1 - fu) + at(v0 + 1, u0 + 1) * fu) * fv
    # The highest of the four vertices around: no ground goes higher than the holomap's own peaks there.
    peak = np.maximum(np.maximum(at(v0, u0), at(v0, u0 + 1)), np.maximum(at(v0 + 1, u0), at(v0 + 1, u0 + 1)))
    return mesh, peak


def anchors():
    """LBA1's holomap markers of its Hamalayi scenes (RESS.HQR entry 8, one per scene): (scene, lat, lon, altitude m)."""
    try:
        d = hqr.entry(LBA1 + "RESS.HQR", 8)
    except OSError:
        return []
    out = []
    for n in (9, 62, 63, 64, 65, 66, 69, 70, 71, 72, 82, 15, 67, 68, 73, 81, 91, 92, 96):
        A, B, size, _ = struct.unpack_from("<hhhh", d, n * 8)
        a = A * 2 * math.pi / 1024; b = B * 2 * math.pi / 1024
        v = (math.sin(b), -math.sin(a) * math.cos(b), math.cos(a) * math.cos(b))
        out.append((n, -math.degrees(math.asin(v[1])), (math.degrees(math.atan2(v[0], v[2])) + 270) % 360, size / HOLO_RADIUS * RADIUS))
    return out


def grow():
    lat, lon = grid()
    rows, cols = lat.shape
    dx_ = RADIUS * np.radians(90 - lat[:, 0]) * np.radians(360.0 / COLUMNS)   # m along, per row
    sampling = (ROW_M, float(np.median(dx_)))
    col = painting()
    land_t, bright_t, brown_t, green_t, teal_t = classify(col)
    # Texture-space smoothing (a texel is 4 by 2 cells) before sampling, so coasts don't follow texels.
    tsm = lambda a, s: ndimage.gaussian_filter(a, s, mode=("nearest", "wrap"))
    soft = sample(tsm(land_t, 0.7), lat, lon)
    # The painting's coasts run along its texel rows for long stretches: noise lets them wander a few meters.
    soft = soft + COAST_WANDER * noise(lat.shape, 16.0, 11, sampling)
    land = (soft > 0.5) & (lat <= LAND_LAT)
    # Water the open sea doesn't reach (dark basins inside the belt) is frozen: low land.
    wlab, wn = ndimage.label(~land)
    open_sea = set(np.unique(wlab[0])) | set(np.unique(wlab[-1])) | {0}
    frozen = ~land & ~np.isin(wlab, list(open_sea))
    land |= frozen
    # Only the ring: the piece that goes all the way around (labels joined across the seam), and pieces near it.
    lab, n = ndimage.label(land)
    seam = {(a, b) for a, b in zip(lab[:, 0], lab[:, -1]) if a and b}
    sizes = ndimage.sum(np.ones_like(lat), lab, range(1, n + 1)) * ROW_M * sampling[1]
    ring = int(np.argmax(sizes)) + 1
    keep = {ring} | {b for a, b in seam if a == ring} | {a for a, b in seam if b == ring}
    near = edt(~np.isin(lab, list(keep)), sampling) < 25.0
    for k in range(1, n + 1):
        if k not in keep and sizes[k - 1] >= MIN_PIECE and near[lab == k].any():
            keep.add(k)
    land = np.isin(lab, list(keep))
    land = ndimage.binary_opening(land, iterations=1)

    paint = np.stack([sample(tsm(col[..., k], 0.6), lat, lon) for k in range(3)], -1)
    bright = sample(tsm(bright_t * land_t, 0.8), lat, lon) / np.maximum(sample(tsm(land_t, 0.8), lat, lon), 0.05)
    brown = sample(tsm(brown_t, 0.8), lat, lon)
    green = sample(tsm(green_t, 0.8), lat, lon)
    speckle = sample(tsm(teal_t + land_t, 1.5), lat, lon)
    holo, holo_peak = holomap_heights(lat, lon)
    holo_peak = blur(np.maximum(holo_peak, OFF_MESH), (8, 40))

    inland = edt(land, sampling)
    offshore = edt(~land, sampling)

    # 2. Wanted height.
    shade = BRIGHT_LOW + (BRIGHT_HIGH - BRIGHT_LOW) * np.clip(bright, 0, 1) ** 1.2
    shade = shade * (1 - 0.6 * brown)
    wanted = np.maximum(holo * shade, OFF_MESH * np.clip(bright, 0, 1) * (1 - brown) + 3.0 * brown)
    wanted += DETAIL * noise(lat.shape, 10.0, 7, sampling) * np.clip(wanted / 10, 0, 1)
    wanted *= np.clip(inland / COAST_RISE, 0, 1)
    wanted0 = wanted.copy()
    # Every column's crest over the wall: a line around the planet along the highest ground (smoothed so it runs
    # unbroken), CREST_WIDTH wide, held over the wall's next level.
    crest_row = np.argmax(np.where(land, blur(wanted, (4, 25)), -1), 0).astype(float)
    crest_row = ndimage.gaussian_filter1d(crest_row, 12, mode="wrap")
    crest = np.abs(np.arange(rows)[:, None] - crest_row[None, :]) * ROW_M <= CREST_WIDTH / 2

    drift = LEVEL_DRIFT * noise(lat.shape, 100.0, 8, sampling)
    sectors = np.arange(COLUMNS) * 32 // COLUMNS
    scale = np.ones(32)
    for _ in range(CALIBRATION_PASSES + 1):
        wanted = wanted0 * np.interp(np.arange(COLUMNS), np.arange(32) * COLUMNS / 32 + COLUMNS / 64, scale, period=COLUMNS)[None, :]
        # Under the holomap's peaks there, softly (a hard cap leaves plateaus).
        wanted = wanted - CAP_SOFTNESS * np.logaddexp(0.0, (wanted - holo_peak) / CAP_SOFTNESS)
        wanted = np.where(crest & land, np.maximum(wanted, WALL + STEP_MAX), wanted)
        h, levels, benches = build(wanted, land, inland, sampling, lat, drift)
        built = np.array([h[:, sectors == k][land[:, sectors == k]].mean() for k in range(32)])
        target = np.array([holo[:, sectors == k][land[:, sectors == k]].mean() for k in range(32)])
        scale = np.clip(scale * target / np.maximum(built, 0.5), 0.5, 4.0)
    # Under water: shallow where the painting speckles (the teal fringe), deep elsewhere.
    shallow = np.clip(speckle / 0.35, 0, 1)
    depth = np.minimum(0.3 + offshore * (0.05 + 0.6 * (1 - shallow)), SHELF_DEPTH + (SKIRT - SHELF_DEPTH) * (1 - shallow))
    h = np.where(land, h, -depth)
    return finish(h, land, frozen, bright, brown, green, sampling, lat, lon, wanted, holo, paint, levels, benches, crest)


def build(wanted, land, inland, sampling, lat, drift):
    # 3. Levels and benches.
    rng = np.random.default_rng(3)
    levels = [SHORE, FIRST_BENCH, WALL]
    top = float(np.percentile(wanted[land], 99.5))
    while levels[-1] + STEP_MIN < top:
        levels.append(levels[-1] + float(rng.uniform(STEP_MIN, STEP_MAX)))
    benches = []
    below = land
    for i, lv in enumerate(levels[1:]):
        lo = levels[i]
        # Levels over the wall drift with place, so the terraces aren't at one height all around.
        shift = drift if lv >= WALL else 0.0
        region = below & (wanted > (lo + lv) / 2 + shift)
        region &= edt(below, sampling) > (COAST_SETBACK if i == 0 else BENCH_SETBACK)
        region = ndimage.binary_opening(region, iterations=1)
        lab, cnt = ndimage.label(region)
        if cnt:
            sz = ndimage.sum(region, lab, range(1, cnt + 1)) * ROW_M * sampling[1]
            region = np.isin(lab, 1 + np.nonzero(sz >= MIN_BENCH_AREA)[0])
        if not region.any():
            break
        benches.append((lo, lv, region, lv == WALL))
        below = region
    ramp_noise = noise(lat.shape, 40.0, 4, sampling)
    ramp_from = np.quantile(ramp_noise, 1 - RAMP_SHARE)
    t = np.clip((ramp_noise - (ramp_from - 0.08)) / 0.16, 0, 1)
    ramp = CLIFF_WIDTH + (RAMP_WIDTH - CLIFF_WIDTH) * t * t * (3 - 2 * t)
    h = np.where(land, SHORE, 0.0)
    for lo, lv, region, is_wall in benches:
        if is_wall:
            # The wall rises in one cell (over 65 degrees at 2 m): no smoothing that could leave a walkable slope.
            h = h + (lv - lo + drift) * region
            continue
        sd = edt(region, sampling) - edt(~region, sampling)
        rise = lv - lo
        h = h + rise * np.clip(blur(sd, EDGE_SMOOTHING) / ramp, 0, 1) * land
    h = np.maximum(h + ROLL * noise(lat.shape, 14.0, 6, sampling) * np.clip(inland / 4, 0, 1), 0.15) * land
    return h, levels, len(benches)


def finish(h, land, frozen, bright, brown, green, sampling, lat, lon, wanted, holo, paint, levels, benches, crest):
    # 4. Ground colors.
    gy, gx = np.gradient(h, ROW_M, sampling[1])
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    b = np.clip(bright, 0, 1)[..., None]
    ice = np.where(b < 0.45, DARK_ICE + (LIGHT_ICE - DARK_ICE) * (b / 0.45),
                   np.where(b < 0.75, LIGHT_ICE + (SNOW_SHADE - LIGHT_ICE) * ((b - 0.45) / 0.3), SNOW_SHADE + (SNOW - SNOW_SHADE) * ((b - 0.75) / 0.25)))
    steep = np.clip((slope - 38) / 17, 0, 1)[..., None]
    br = np.clip(brown, 0, 1)[..., None]; gr = np.clip(green, 0, 1)[..., None] * (1 - br)
    ground = ice * (1 - br - gr) + BROWN * br + GREEN * gr
    cliff = DARK_ICE * (1 - br) + BROWN_CLIFF * br
    ground = ground * (1 - steep) + cliff * steep
    under = np.clip(-h / SHELF_DEPTH, 0, 1)[..., None]
    sea = SHELF * (1 - under) + DEEP * under
    ground = np.where((frozen & (slope < 20))[..., None], FROZEN, ground)
    color = np.where(land[..., None], ground, sea)
    return dict(lat=lat, lon=lon, h=h, land=land, wanted=wanted, holo=holo, slope=slope, color=color, paint=paint,
                levels=levels, benches=benches, sampling=sampling, crest=crest)


def walk_through(g):
    """Can anyone walk from the islands' side's water to the far side's? Cells joined when the step between them is
    under WALK_LIMIT; columns wrap."""
    h, land = g["h"], g["land"]
    dy, dx = g["sampling"]
    lim = math.tan(math.radians(WALK_LIMIT))
    ok_v = np.abs(np.diff(h, axis=0)) / dy < lim
    ok_h = np.abs(np.diff(np.concatenate([h, h[:, :1]], 1), axis=1)) / dx < lim
    rows, cols = h.shape
    reach = np.zeros_like(land)
    wlab, _ = ndimage.label(~land)
    start = np.isin(wlab, list(set(np.unique(wlab[0])) - {0}))
    reach[start] = True
    frontier = start.copy()
    while frontier.any():
        new = np.zeros_like(reach)
        new[1:] |= frontier[:-1] & ok_v
        new[:-1] |= frontier[1:] & ok_v
        new |= np.roll(frontier & ok_h, 1, axis=1)
        new |= np.roll(frontier, -1, axis=1) & ok_h
        frontier = new & ~reach
        reach |= frontier
    far = np.isin(wlab, list(set(np.unique(wlab[-1])) - {0}))
    return reach, (reach & far).any()


def hillshade(h, sampling):
    gy, gx = np.gradient(h, sampling[0], sampling[1])
    n = np.dstack([-gx, -gy, np.ones_like(h)]); n /= np.linalg.norm(n, axis=2, keepdims=True)
    light = np.array([-0.5, -0.6, 0.62]); light /= np.linalg.norm(light)
    return np.clip(n @ light, 0, 1)


def sheets(g):
    os.makedirs(OUT, exist_ok=True)
    lat, lon, h = g["lat"], g["lon"], g["h"]
    shade = hillshade(h, g["sampling"])
    hv = np.clip(h / 70, 0, 1)
    height_img = (np.dstack([hv, hv, hv]) * 0.6 + shade[..., None] * 0.4) * 255
    height_img[~g["land"]] = (20, 40, 70)
    colour = g["color"] * (0.55 + 0.45 * shade[..., None])
    marks = anchors()
    rows = h.shape[0]
    for name, lo0, lo1 in (("hamalayi", 335, 80), ("east", 75, 185), ("south", 180, 290), ("west", 285, 340)):
        cols = [c for c in range(COLUMNS) if (lon[0, c] - lo0) % 360 <= (lo1 - lo0) % 360]
        cols.sort(key=lambda c: (lon[0, c] - lo0) % 360)
        panels = [g["paint"][:, cols], colour[:, cols], height_img[:, cols]]
        W = len(cols); S = 2 if W < 700 else 1
        sheet = Image.new("RGB", (W * S, (rows * S + 16) * 3), (0, 0, 0))
        for i, p in enumerate(panels):
            im = Image.fromarray(np.clip(p, 0, 255).astype(np.uint8)).resize((W * S, rows * S), Image.NEAREST)
            sheet.paste(im, (0, i * (rows * S + 16) + 16))
            dr = ImageDraw.Draw(sheet)
            dr.text((4, i * (rows * S + 16) + 2), ["holomap painting", "belt colors (hillshaded)", "belt heights 0-70 m"][i] + f"  lon {lo0}..{lo1}, lat {LAT_TOP}..{LAT_BOTTOM}", fill=(255, 255, 255))
            for (n, la, lo_, alt) in marks:
                if (lo_ - lo0) % 360 > (lo1 - lo0) % 360:
                    continue
                x = ((lo_ - lo0) % 360) / ((lo1 - lo0) % 360) * W * S
                y = (LAT_TOP - la) / (LAT_TOP - LAT_BOTTOM) * rows * S + i * (rows * S + 16) + 16
                dr.ellipse((x - 3, y - 3, x + 3, y + 3), outline=(255, 0, 255))
                if i == 2:
                    r_ = int(np.clip((LAT_TOP - la) / (LAT_TOP - LAT_BOTTOM) * rows, 0, rows - 1)); c_ = int(lo_ / 360 * COLUMNS) % COLUMNS
                    dr.text((x + 4, y - 6), f"{n}:{alt:.0f}/{h[r_, c_]:.0f}", fill=(255, 0, 255))
        sheet.save(os.path.join(OUT, f"sheet_{name}.png"))


def report(g):
    h, land, holo = g["h"], g["land"], g["holo"]
    dy, dx = g["sampling"]
    area = land.sum() * dy * dx
    width = land.sum(0) * dy
    print(f"land {area:,.0f} m2, width median {np.median(width):.0f} m (min {width.min():.0f}, max {width.max():.0f})")
    print(f"levels {' '.join(f'{l:.1f}' for l in g['levels'][:g['benches'] + 1])} ({g['benches']} benches)")
    print(f"heights p50/90/99/max {' '.join(f'{np.percentile(h[land], p):.1f}' for p in (50, 90, 99))} {h[land].max():.1f}; "
          f"holomap mesh over the same land p50/90/max {np.percentile(holo[land], 50):.1f} {np.percentile(holo[land], 90):.1f} {holo[land].max():.1f}")
    # Per 11.25 degrees: mean height against the holomap's mean.
    blocks = []
    for c in range(32):
        sl = slice(c * COLUMNS // 32, (c + 1) * COLUMNS // 32)
        m = land[:, sl]
        blocks.append((h[:, sl][m].mean() if m.any() else 0, holo[:, sl][m].mean() if m.any() else 0, h[:, sl][m].max() if m.any() else 0))
    print("per 11.25 deg (built mean/holomap mean/max): " + " ".join(f"{a:.0f}/{b:.0f}/{c:.0f}" for a, b, c in blocks))
    s = g["slope"][land]
    print("slopes 0-10/10-30/30-55/55+: " + " ".join(f"{((s >= a) & (s < b)).mean():.0%}" for a, b in ((0, 10), (10, 30), (30, 55), (55, 91))))
    reach, through = walk_through(g)
    print("walkable from the islands' water to the far side's:", through)
    img = np.dstack([reach * 255, g["land"] * 120, g["crest"] * 200]).astype(np.uint8)
    Image.fromarray(img).save(os.path.join(OUT, "reach.png"))
    for (n, la, lo_, alt) in anchors():
        r_ = int(np.clip((LAT_TOP - la) / (LAT_TOP - LAT_BOTTOM) * h.shape[0], 0, h.shape[0] - 1)); c_ = int(lo_ / 360 * COLUMNS) % COLUMNS
        print(f"  LBA1 scene {n:3d} at {la:6.1f} {lo_:6.1f}: marker {alt:5.1f} m, ground {h[r_, c_]:5.1f} m{'' if land[r_, c_] else ' (water)'}")


if __name__ == "__main__":
    g = grow()
    report(g)
    sheets(g)
    np.savez_compressed(os.path.join(OUT, "heights.npz"), h=g["h"].astype(np.float32), land=g["land"])
