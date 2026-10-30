"""Measures LBA2's island terrain, for a grammar that grows the globe's stub islands like the game's own.

Usage: terrain_stats.py [out dir]   (default Saved/LookDev/terrain)

Reads every *.ILE (LBA_EXT.H / LOADISLE.CPP: 16 x 16 map of cubes, 64 x 64 cells of 512 units, 65 x 65 int16 heights,
two T_HALF_POLY triangles per cell). Unreal cm = units / 8, so a cell is 0.64 m. A cell is land when both its
triangles are drawn and not water at sea level (as the importer's ClassifyGroundTriangle); everything else is sea.

Per island: land area, highest point, the coast profile (height percentiles by distance inland), slope histogram,
how often the coast is a beach or a cliff, ground classes (sand, grass, rock, earth, snow, paved: from the ground
texture's colors) by height and distance inland, flat ground's height levels (terraces). Writes stats.json, a text
report (report.txt) and <island>.png (heights | slopes | ground classes).
"""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/terrain")
GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2").rstrip("/\\") + "/"
CELL_M = 512 / 800.0
HEIGHT_M = 1 / 800.0

# RESS.HQR's island palettes (LBAXpl.cpp): RESS_XPL0 + island number, Citabau and others on the interiors' one.
XPL_ORDER = ["CITADEL", "SENDELL", "DESERT", "EMERAUDE", "OTRINGAL", "CELEBRAT", "PLATFORM", "MOSQUIBE", "KNARTAS",
             "ILOTCX", "ASCENCE", "SOUSCELB"]
ISLANDS = ["CITABAU", "CITADEL", "DESERT", "EMERAUDE", "OTRINGAL", "CELEBRAT", "CELEBRA2", "PLATFORM", "MOSQUIBE",
           "KNARTAS", "ILOTCX", "ASCENCE", "SOUSCELB", "MOON"]
CLASSES = ["sand", "grass", "rock", "earth", "snow", "paved", "dark"]
CLASS_COLORS = [(230, 205, 130), (70, 160, 60), (130, 130, 140), (140, 95, 55), (245, 245, 255), (200, 120, 200), (30, 30, 50)]
DIST_BINS = [0, 1, 2, 3, 5, 8, 12, 20, 30, 50, 1000]   # m inland
HEIGHT_BINS = [0, 0.5, 1, 2, 4, 6, 9, 13, 20, 1000]   # m


def palette(name):
    base = {"CELEBRA2": "CELEBRAT", "MOON": "EMERAUDE"}.get(name, name)
    entry = 27 + XPL_ORDER.index(base) if base in XPL_ORDER and name != "CITABAU" else 42
    d = hqr.entry(GAME + "RESS.HQR", entry)
    off = int.from_bytes(d[4:8], "little")
    return np.frombuffer(d[off:off + 768], np.uint8).reshape(256, 3).astype(np.float32)


def corners(sens, tri):
    table = [[[(0, 0), (0, 1), (1, 1)], [(1, 1), (1, 0), (0, 0)]], [[(1, 0), (0, 0), (0, 1)], [(0, 1), (1, 1), (1, 0)]]]
    return table[sens & 1][tri & 1]


def load(name):
    path = GAME + name + ".ILE"
    m = hqr.entry(path, 0)
    ground_tex = np.frombuffer(hqr.entry(path, 1)[:65536], np.uint8).reshape(256, 256)
    pal = palette(name)
    cubes = [(i % 16, i // 16, m[i] & 127) for i in range(256) if m[i] & 127]
    x0 = min(c[0] for c in cubes); z0 = min(c[1] for c in cubes)
    nx = (max(c[0] for c in cubes) - x0 + 1) * 64; nz = (max(c[1] for c in cubes) - z0 + 1) * 64
    heights = np.zeros((nz + 1, nx + 1), np.float32)
    land = np.zeros((nz, nx), bool)
    present = np.zeros((nz, nx), bool)
    color = np.zeros((nz, nx, 3), np.float32)
    for cx, cz, cid in cubes:
        base = 3 + 6 * (cid - 1)
        h = np.frombuffer(hqr.entry(path, base + 4)[:65 * 65 * 2], "<i2").reshape(65, 65).astype(np.float32)
        tris = np.frombuffer(hqr.entry(path, base + 2)[:64 * 64 * 2 * 4], "<u4").reshape(64, 64, 2)
        texdef = hqr.entry(path, base + 3)
        uv = np.frombuffer(texdef[:len(texdef) // 12 * 12], "<u2").reshape(-1, 3, 2).astype(np.float32) / 256.0
        ox = (cx - x0) * 64; oz = (cz - z0) * 64
        heights[oz:oz + 65, ox:ox + 65] = h
        for z in range(64):
            for x in range(64):
                ok = True; cols = []
                sens = (int(tris[z, x, 0]) >> 16) & 1
                for t in range(2):
                    raw = int(tris[z, x, t])
                    tex = (raw >> 4) & 3; poly = (raw >> 6) & 3; jeu = (raw >> 12) & 15
                    if not tex and not poly:
                        ok = False; continue
                    sea = all(h[z + cz_, x + cx_] == 0 for cx_, cz_ in corners(sens, t))
                    if sea and jeu:
                        ok = False
                    flat = pal[(raw & 15) * 16 + 11]
                    ti = raw >> 19
                    if tex and ti < len(uv):
                        c = uv[ti].mean(0)
                        idx = ground_tex[int(c[1]) & 255, int(c[0]) & 255]
                        cols.append(pal[idx] if idx else flat)
                    else:
                        cols.append(flat)
                present[oz + z, ox + x] = True
                land[oz + z, ox + x] = ok
                if cols:
                    color[oz + z, ox + x] = np.mean(cols, 0)
    return heights * HEIGHT_M, land, color


def classify(color):
    """Ground class of a color (sRGB 0..255): the hue and brightness families the island textures use."""
    r, g, b = color[..., 0], color[..., 1], color[..., 2]
    lum = (r + g + b) / 3
    mx = color.max(-1); mn = color.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1)
    out = np.full(r.shape, 3, np.int8)                          # earth (warm, darker)
    out[(sat < 0.18)] = 2                                       # rock (grey)
    out[(g > r * 1.05) & (g > b)] = 1                           # grass
    out[(r > g) & (g > b * 1.15) & (lum > 120) & (sat > 0.18)] = 0   # sand
    out[(lum > 190) & (sat < 0.2)] = 4                          # snow / pale stone
    out[(b > r * 1.1) & (b > g) & (lum > 60)] = 5               # blue-ish paving, roofs, tiles
    out[lum < 40] = 6
    return out


def analyse(name):
    h, land, color = load(name)
    cell_h = (h[:-1, :-1] + h[1:, :-1] + h[:-1, 1:] + h[1:, 1:]) / 4
    gz, gx = np.gradient(cell_h, CELL_M)
    slope = np.degrees(np.arctan(np.hypot(gx, gz)))
    inland = ndimage.distance_transform_edt(land) * CELL_M
    # Only the main landmasses: specks under 50 m2 are piers, rocks, boats.
    lab, n = ndimage.label(land)
    sizes = ndimage.sum(land, lab, range(1, n + 1)) * CELL_M ** 2
    keep = np.isin(lab, [i + 1 for i, s in enumerate(sizes) if s >= 50])
    cls = classify(color)
    L = keep
    st = dict(name=name, area=float(L.sum() * CELL_M ** 2), cells=int(L.sum()), pieces=int((sizes >= 50).sum()),
              highest=float(cell_h[L].max()) if L.any() else 0.0,
              height_pct={p: float(np.percentile(cell_h[L], p)) for p in (10, 25, 50, 75, 90, 99)},
              max_inland=float(inland[L].max()))
    prof = []
    for a, b in zip(DIST_BINS[:-1], DIST_BINS[1:]):
        m = L & (inland > a) & (inland <= b)
        if m.sum() < 20:
            continue
        hh = cell_h[m]; ss = slope[m]
        prof.append(dict(d0=a, d1=b, n=int(m.sum()), h25=float(np.percentile(hh, 25)), h50=float(np.median(hh)),
                         h75=float(np.percentile(hh, 75)), h90=float(np.percentile(hh, 90)),
                         slope50=float(np.median(ss)), cls=[float((cls[m] == k).mean()) for k in range(len(CLASSES))]))
    st["profile"] = prof
    st["slope_hist"] = [float(((slope[L] >= a) & (slope[L] < b)).mean()) for a, b in
                        zip([0, 5, 10, 20, 30, 45, 60], [5, 10, 20, 30, 45, 60, 91])]
    # Coast type along the shore: height reached 3 m inland, measured on the shore cells' inland neighbours.
    shore = L & (inland <= 1.0)
    at3 = ndimage.maximum_filter(np.where(L & (inland > 2.5) & (inland <= 3.5), cell_h, -1), size=9)
    hs = at3[shore]; hs = hs[hs >= 0]
    st["coast_at3m"] = {p: float(np.percentile(hs, p)) for p in (10, 25, 50, 75, 90)} if len(hs) else {}
    st["coast_cliff_frac"] = float((hs > 2.0).mean()) if len(hs) else 0.0     # rises > 2 m in 3 m: cliff/wall
    st["coast_beach_frac"] = float((hs < 0.6).mean()) if len(hs) else 0.0     # under 0.6 m: beach
    st["shore_height"] = {p: float(np.percentile(cell_h[shore], p)) for p in (10, 50, 90)} if shore.any() else {}
    st["cls_total"] = [float((cls[L] == k).mean()) for k in range(len(CLASSES))]
    st["cls_by_height"] = []
    for a, b in zip(HEIGHT_BINS[:-1], HEIGHT_BINS[1:]):
        m = L & (cell_h >= a) & (cell_h < b)
        if m.sum() >= 20:
            st["cls_by_height"].append(dict(h0=a, h1=b, frac=float(m.mean() / max(L.mean(), 1e-9)),
                                            cls=[float((cls[m] == k).mean()) for k in range(len(CLASSES))]))
    st["cls_slope50"] = [float(np.median(slope[L & (cls == k)])) if (L & (cls == k)).sum() > 20 else None for k in range(len(CLASSES))]
    st["cls_color"] = [[float(v) for v in np.median(color[L & (cls == k)], 0)] if (L & (cls == k)).sum() > 20 else None for k in range(len(CLASSES))]
    # Terraces: flat ground's heights, 0.25 m bins; levels = local maxima holding > 3% of the flat area.
    flat = L & (slope < 6)
    st["flat_frac"] = float(flat.sum() / max(L.sum(), 1))
    hist, edges = np.histogram(cell_h[flat], bins=np.arange(0, max(st["highest"], 1) + 0.5, 0.25))
    tot = max(hist.sum(), 1)
    sm = np.convolve(hist, [1, 2, 1], "same") / 4
    st["terraces"] = [round(float(edges[i] + 0.125), 2) for i in range(1, len(sm) - 1)
                      if sm[i] >= sm[i - 1] and sm[i] > sm[i + 1] and hist[max(i - 1, 0):i + 2].sum() / tot > 0.03]
    # Hill shape: height against distance inland, normalised by the island's own reach.
    st["peak_inland"] = float(inland[L][np.argmax(cell_h[L])]) if L.any() else 0.0
    picture(name, cell_h, slope, cls, L)
    return st


def picture(name, cell_h, slope, cls, L):
    hmax = max(float(cell_h[L].max()), 1.0) if L.any() else 1.0
    hn = np.clip(cell_h / hmax, 0, 1)
    a = np.zeros(L.shape + (3,), np.uint8); a[...] = (20, 40, 80)
    a[L] = (np.stack([hn, hn, hn], -1)[L] * 220 + 30).astype(np.uint8)
    s = np.zeros_like(a); s[...] = (20, 40, 80)
    sn = np.clip(slope / 60, 0, 1)
    s[L] = (np.stack([sn, 1 - sn, 0.2 + 0 * sn], -1)[L] * 230).astype(np.uint8)
    c = np.zeros_like(a); c[...] = (20, 40, 80)
    for k, col in enumerate(CLASS_COLORS):
        c[L & (cls == k)] = col
    img = np.concatenate([a, s, c], 1)
    Image.fromarray(img).save(os.path.join(OUT, name + ".png"))


def report(all_stats):
    lines = []
    for st in all_stats:
        lines.append(f"== {st['name']}: {st['area']:.0f} m2 in {st['pieces']} pieces, highest {st['highest']:.1f} m, "
                     f"reach {st['max_inland']:.1f} m inland, peak {st['peak_inland']:.1f} m inland, flat {st['flat_frac']:.0%}")
        lines.append("   heights p10/25/50/75/90/99: " + " ".join(f"{v:.1f}" for v in st["height_pct"].values()))
        lines.append(f"   coast: shore h p10/50/90 {' '.join(f'{v:.2f}' for v in st['shore_height'].values())}; "
                     f"h at 3 m p10..90 {' '.join(f'{v:.1f}' for v in st['coast_at3m'].values())}; "
                     f"beach {st['coast_beach_frac']:.0%}, cliff {st['coast_cliff_frac']:.0%}")
        lines.append("   slopes 0-5/5-10/10-20/20-30/30-45/45-60/60+: " + " ".join(f"{v:.0%}" for v in st["slope_hist"]))
        lines.append("   classes " + " ".join(f"{n} {v:.0%}" for n, v in zip(CLASSES, st["cls_total"])))
        lines.append("   class colors " + " ".join(f"{n}({','.join(str(int(x)) for x in c)})" for n, c in zip(CLASSES, st["cls_color"]) if c))
        lines.append("   class slope50 " + " ".join(f"{n} {v:.0f}" for n, v in zip(CLASSES, st["cls_slope50"]) if v is not None))
        lines.append("   terraces (m): " + " ".join(str(t) for t in st["terraces"]))
        lines.append("   inland m    n      h25   h50   h75   h90  slope | " + " ".join(f"{c[:5]:>5}" for c in CLASSES))
        for p in st["profile"]:
            lines.append(f"   {p['d0']:>3}-{p['d1']:<4} {p['n']:>6} {p['h25']:5.1f} {p['h50']:5.1f} {p['h75']:5.1f} {p['h90']:5.1f} {p['slope50']:5.0f} | "
                         + " ".join(f"{v:5.0%}" for v in p["cls"]))
        lines.append("   by height:")
        for p in st["cls_by_height"]:
            lines.append(f"   {p['h0']:>4}-{p['h1']:<5} {p['frac']:5.0%} | " + " ".join(f"{v:5.0%}" for v in p["cls"]))
    return "\n".join(lines)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    stats = []
    for name in ISLANDS:
        if os.path.exists(GAME + name + ".ILE"):
            stats.append(analyse(name))
    json.dump(stats, open(os.path.join(OUT, "stats.json"), "w"), indent=1)
    text = report(stats)
    open(os.path.join(OUT, "report.txt"), "w").write(text)
    print(text)
