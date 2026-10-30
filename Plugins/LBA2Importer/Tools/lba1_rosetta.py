"""Lays LBA1's outdoor scenes over the LBA2 island that replaced them (Citadel, desert) and measures the change.

Usage: lba1_rosetta.py [out dir]   (default Saved/LookDev/rosetta)

LBA1 builds an island from separate iso scenes (64 x 64 cells of 512 units) joined by exits at their edges; an exit
moves the hero by info - mins (lba1.py), which places every scene of the island in one frame. LBA2 builds it from
cubes of 32768 units (island x = cube x * 32768 + scene x; Unreal cm = island units / 8, X = x, Y = z). Buildings
both games kept are matched by their doors (cube change zones into the interior) and a similarity transform
(scale, turn, offset) is fitted to them.

Writes <island>.png (LBA2 coast + doors | LBA1 plan + doors | LBA1 laid over LBA2 by the fit) and rosetta.json, and
prints: the fit and each door's residual under three models (offset only, offset + scale, + turn), door-to-door
distance ratios (LBA2 / LBA1), which LBA1 scene covers each LBA2 cube, and land areas. Needs planet.json from
dump_planet.py for LBA2's coastlines.
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lba1
import dump_scenes as lba2

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/rosetta")
PLANET_JSON = os.path.join(PROJECT, "Saved/LookDev/holomap/planet.json")
CUBE = 32768
UNITS_PER_M = 800.0  # Unreal cm = units / 8
PX = 128.0           # island units per image pixel

# LBA1 outdoor scenes, LBA2 outdoor cubes, and doors of the same building: (LBA1 interior, LBA2 interior, name,
# sure). Unsure pairs are only measured against the fit of the sure ones.
ISLANDS = {
    "CITABAU": dict(lba1=[1, 3, 2, 4, 6], lba2=list(range(42, 51)), doors=[
        (5, 0, "Twinsen's house", True), (14, 3, "Tavern = Chez Luc", True), (7, 22, "Pharmacy", True),
        (104, 8, "Ticket office", True), (35, 7, "Warehouse ~ Paul's house", False),
        (20, 9, "Architect's ~ Neighbour's house", False)]),
    # The well into the Temple of Bu: LBA1's hero script changes to scene 8 while Twinsen stands in scenic zone 1
    # of "Outside the Temple of Bu"; LBA2's is the door to the Well (127). The ferry: LBA1's model 27, found only in
    # the ferry ports (Citadel harbour, this camp, Proxim City, Port Belooga), moored in the camp; LBA2's ferry
    # (body 165) in Town Square. Two points fix any fit exactly; the sheet turns LBA1 about the well, unscaled.
    "DESERT": dict(lba1=[36, 39], lba2=list(range(55, 74)), model="pivot", doors=[
        (("zone", 36, 1), 127, "Well into the Temple of Bu", True),
        (("actor", 39, 1), ("actor", 60, 30), "Ferry", True)]),
}


_names = None


def lba2_name(n):
    """lba2remake's name for an LBA2 scene (Resources/CommunityMetadata/lba2_locations.json)."""
    global _names
    if _names is None:
        _names = {}
        def walk(node):
            for c in node.get("children", []):
                _names[c["scene"]] = c["name"]; walk(c)
        loc = json.load(open(os.path.join(TOOLS, "../Resources/CommunityMetadata/lba2_locations.json"), encoding="utf-8"))
        for planet in loc["planets"]:
            for isl in planet["islands"]:
                walk(isl)
    return _names.get(n, "")


def lba1_frame(scenes):
    """Offset (x, y, z) of each LBA1 scene in the first one's frame, from its edge exits; plus every exit found."""
    offsets = {scenes[0]: np.zeros(3)}; links = []
    parsed = {n: lba1.parse_scene(n) for n in scenes}
    todo = [scenes[0]]
    while todo:
        a = todo.pop(0)
        for z in parsed[a]["zones"]:
            b = z["num"]
            if z["type"] != lba1.ZONE_CUBE or b not in parsed:
                continue
            d = np.array(z["mins"], float) - np.array(z["info"][:3], float)
            links.append((a, b, d))
            if b not in offsets:
                offsets[b] = offsets[a] + d; todo.append(b)
    return offsets, parsed, links


def lba1_doors(offsets, parsed):
    """Door point (x, y, z) per interior scene: center of its lowest cube change zone, in the island frame; plus
    ("zone", scene, number) for each scenic zone's center (entrances the scripts handle) and ("actor", scene, index)
    for each actor's position."""
    doors = {}
    for n, s in parsed.items():
        if n not in offsets:
            continue
        for i, a in enumerate(s["actors"]):
            doors[("actor", n, i + 1)] = offsets[n] + np.array(a["pos"], float)
        for z in s["zones"]:
            if z["type"] == 2:
                doors[("zone", n, z["num"])] = offsets[n] + (np.array(z["mins"], float) + np.array(z["maxs"], float)) / 2
            if z["type"] != lba1.ZONE_CUBE or z["num"] in parsed:
                continue
            c = offsets[n] + (np.array(z["mins"], float) + np.array(z["maxs"], float)) / 2
            if z["num"] not in doors or c[1] < doors[z["num"]][1]:
                doors[z["num"]] = c
    return doors


def lba2_doors(scenes):
    """Door point per interior scene (and ("actor", scene, index) per actor) and cube origin per outdoor scene, in
    LBA2 island units (x, y, z)."""
    doors = {}; cubes = {}
    for n in scenes:
        s = lba2.parse(n)
        o = np.array([s["cubex"] * CUBE, 0, s["cubey"] * CUBE], float)
        cubes[n] = o
        for i, a in enumerate(s["actors"]):
            doors[("actor", n, i + 1)] = o + np.array(a["pos"], float)
        for z in s["zones"]:
            if z["type"] != 0 or z["num"] in scenes:
                continue
            b = z["box"]
            c = o + (np.array(b[:3], float) + np.array(b[3:], float)) / 2
            if z["num"] not in doors or c[1] < doors[z["num"]][1]:
                doors[z["num"]] = c
    return doors, cubes


def fit_similarity(src, dst):
    """Scale s, rotation R (2x2, no mirror) and offset t with dst ~ s R src + t (Umeyama), on (x, z) points."""
    ms, md = src.mean(0), dst.mean(0)
    a, b = src - ms, dst - md
    u, sig, vt = np.linalg.svd(b.T @ a / len(src))
    d = np.diag([1, np.sign(np.linalg.det(u @ vt))])
    r = u @ d @ vt
    s = np.trace(np.diag(sig) @ d) / (a ** 2).sum(1).mean()
    return s, r, md - s * r @ ms


def fit_models(src, dst):
    """(name, scale, rotation, offset) for: offset only, offset + scale, offset + turn, offset + scale + turn, and
    pivot (the turn, about the first point, which stays exact)."""
    out = []
    for name in ("offset", "scale", "turn", "similarity", "pivot"):
        if name == "similarity":
            s, r, _ = fit_similarity(src, dst)
        elif name in ("turn", "pivot"):
            _, r, _ = fit_similarity(src, dst); s = 1.0
            if name == "pivot":
                out.append((name, s, r, dst[0] - r @ src[0])); continue
        else:
            a, b = src - src.mean(0), dst - dst.mean(0)
            s, r = (1.0 if name == "offset" else (a * b).sum() / (a * a).sum()), np.eye(2)
        out.append((name, s, r, dst.mean(0) - s * r @ src.mean(0)))
    return out


def composite(offsets):
    """LBA1 plan of all scenes in one image grid [z, x] of cells; later scenes draw over earlier ones only where
    they are higher. Returns rgb, land mask, origin (units) of cell 0, 0."""
    cells = {n: (np.round(o[[0, 2]] / lba1.BRICK_XZ).astype(int), int(round(o[1] / lba1.BRICK_Y)))
             for n, o in offsets.items()}
    lo = np.min([c for c, _ in cells.values()], 0); hi = np.max([c for c, _ in cells.values()], 0) + 64
    shape = (hi[1] - lo[1], hi[0] - lo[0])
    height = np.full(shape, -999); rgb = np.zeros(shape + (3,)); land = np.zeros(shape, bool)
    for n, (c, dy) in cells.items():
        h, col, water = lba1.plan(n)
        z0, x0 = c[1] - lo[1], c[0] - lo[0]
        win = (slice(z0, z0 + 64), slice(x0, x0 + 64))
        hh = np.where(h >= 0, h + dy, -999)
        take = hh > height[win]
        height[win][take] = hh[take]; rgb[win][take] = col[take]; land[win][take] = ~water[take]
    return rgb, land, height, lo * lba1.BRICK_XZ


def draw_marks(draw, points, to_px, color, labels=None):
    for i, p in enumerate(points):
        x, y = to_px(p)
        draw.ellipse((x - 4, y - 4, x + 4, y + 4), fill=color, outline=(0, 0, 0))
        if labels:
            draw.text((x + 6, y - 6), labels[i], fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))


def main():
    os.makedirs(OUT, exist_ok=True)
    planet = {i["name"]: i for i in json.load(open(PLANET_JSON))["islands"]}
    report = {}
    for island, cfg in ISLANDS.items():
        offsets, parsed, links = lba1_frame(cfg["lba1"])
        d1 = lba1_doors(offsets, parsed)
        d2, cubes = lba2_doors(cfg["lba2"])
        print(f"== {island}: LBA1 scenes", {n: tuple(int(v) for v in o) for n, o in offsets.items()})
        for a, b, d in links:
            print(f"   exit {a}->{b} {tuple(int(v) for v in d)}  vs frame {tuple(int(v) for v in offsets[b] - offsets[a])}")
        bank = parsed[cfg["lba1"][0]]["bank"]
        print("   LBA1 doors:", {lba1.NAMES.get(k, k): tuple(int(v) for v in p) for k, p in d1.items()
                                 if not isinstance(k, tuple) and lba1.parse_scene(k)["bank"] == bank})
        pairs = [(a, b, name, sure) for a, b, name, sure in cfg["doors"] if a in d1 and b in d2]
        sure = [(a, b) for a, b, _, s in pairs if s]
        models = []; s = None
        if len(sure) >= 2:
            src = np.array([d1[a][[0, 2]] for a, _ in sure]); dst = np.array([d2[b][[0, 2]] for _, b in sure])
            for name, ms, mr, mt in fit_models(src, dst):
                e = np.array([d2[b][[0, 2]] - (ms * mr @ d1[a][[0, 2]] + mt) for a, b, _, _ in pairs]) / UNITS_PER_M
                fitted_rms = float(np.sqrt(np.mean([(v ** 2).sum() for v, p in zip(e, pairs) if p[3]])))
                turn = np.degrees(np.arctan2(mr[1, 0], mr[0, 0]))
                print(f"   model {name:10s}: scale {ms:.3f}, turn {turn:5.1f} deg, rms of sure doors {fitted_rms:5.2f} m; "
                      + ", ".join(f"{p[2].split(' ')[0]} {np.hypot(*v):.1f}" for v, p in zip(e, pairs)))
                models.append(dict(model=name, scale=ms, turn_deg=turn, rms_m=fitted_rms))
                if name == cfg.get("model", "similarity"):
                    s, r, t = ms, mr, mt
            print("   door distances LBA2 / LBA1:")
            for i in range(len(pairs)):
                for j in range(i + 1, len(pairs)):
                    a1 = np.linalg.norm(d1[pairs[i][0]][[0, 2]] - d1[pairs[j][0]][[0, 2]])
                    a2 = np.linalg.norm(d2[pairs[i][1]][[0, 2]] - d2[pairs[j][1]][[0, 2]])
                    print(f"     {pairs[i][2][:24]:24s} - {pairs[j][2][:24]:24s} {a1 / UNITS_PER_M:5.1f} -> "
                          f"{a2 / UNITS_PER_M:5.1f} m  x{a2 / a1:.2f}")
        else:
            # One shared point: offset only.
            (a, b), = sure
            s, r = 1.0, np.eye(2); t = d2[b][[0, 2]] - d1[a][[0, 2]]
        turn = np.degrees(np.arctan2(r[1, 0], r[0, 0]))
        print(f"   fit ({cfg.get('model', 'similarity') if models else 'offset'}): scale {s:.3f}, turn {turn:.1f} deg, "
              f"offset {t.round(0)} units")
        res = []
        for a, b, name, ok in pairs:
            p = s * r @ d1[a][[0, 2]] + t; e = d2[b][[0, 2]] - p
            res.append(dict(name=name, sure=ok, lba1=d1[a].tolist(), lba2=d2[b].tolist(),
                            residual_m=(e / UNITS_PER_M).round(2).tolist(), dist_m=float(np.hypot(*e) / UNITS_PER_M)))
            print(f"   {'  ' if ok else '? '}{name:34s} residual {np.hypot(*e) / UNITS_PER_M:6.2f} m  "
                  f"(dx {e[0] / UNITS_PER_M:+.2f}, dz {e[1] / UNITS_PER_M:+.2f})  height {d1[a][1]:.0f} -> {d2[b][1]:.0f}")

        # Which LBA1 scene covers each LBA2 cube, under the fit (sampled on a 16 x 16 grid per cube).
        cover = {}
        uv = (np.mgrid[0:16, 0:16].reshape(2, -1).T + 0.5) * CUBE / 16
        for n, o in cubes.items():
            pts = (o[[0, 2]] + uv - t) @ r / s
            hits = {m: int(np.all((pts >= om[[0, 2]]) & (pts < om[[0, 2]] + CUBE), 1).sum()) for m, om in offsets.items()}
            best = max(hits, key=hits.get)
            cover[n] = (best, hits[best] / len(uv)) if hits[best] else (None, 0.0)
            print(f"   LBA2 {n} {lba2_name(n):28s} <- " + (f"LBA1 {best} {lba1.NAMES.get(best, '')} ({hits[best] / len(uv):.0%})"
                                                       if hits[best] else "new"))

        rgb, land, height, origin = composite(offsets)
        cell_m2 = (lba1.BRICK_XZ / UNITS_PER_M) ** 2
        coast = np.array(planet[island]["coast"]) * 8.0  # Unreal cm -> island units
        area2 = 0.5 * abs(np.dot(coast[:, 0], np.roll(coast[:, 1], 1)) - np.dot(coast[:, 1], np.roll(coast[:, 0], 1)))
        area1 = land.sum() * cell_m2
        print(f"   land: LBA1 {area1:.0f} m2 (scenes' grids, all non-water columns), LBA2 {area2 / UNITS_PER_M ** 2:.0f} m2"
              f" (coastline)")

        # Image frame: LBA2 island units, x right, z down, around the coast and the fitted LBA1 frame.
        corners = [origin + np.array([u, v]) * lba1.BRICK_XZ for u in (0, rgb.shape[1]) for v in (0, rgb.shape[0])]
        pts = np.vstack([coast, [s * r @ c + t for c in corners]])
        lo = pts.min(0) - 4096; hi = pts.max(0) + 4096
        W, H = (int((hi - lo)[0] / PX), int((hi - lo)[1] / PX))
        to_px = lambda p: ((p[0] - lo[0]) / PX, (p[-1] - lo[1]) / PX)

        def base():
            img = Image.new("RGB", (W, H), (38, 84, 128)); dr = ImageDraw.Draw(img)
            dr.polygon([to_px(p) for p in coast], fill=(150, 165, 110), outline=(230, 220, 170))
            for cx in range(int(lo[0] // CUBE) + 1, int(hi[0] // CUBE) + 1):
                dr.line([to_px((cx * CUBE, lo[1])), to_px((cx * CUBE, hi[1]))], fill=(70, 110, 150))
            for cz in range(int(lo[1] // CUBE) + 1, int(hi[1] // CUBE) + 1):
                dr.line([to_px((lo[0], cz * CUBE)), to_px((hi[0], cz * CUBE))], fill=(70, 110, 150))
            for n, o in cubes.items():
                dr.text(to_px(o[[0, 2]] + 900), f"{n} {lba2_name(n)}", fill=(200, 220, 240))
            return img

        # LBA1 plan resampled into the image frame (inverse of the fit).
        ys, xs = np.mgrid[0:H, 0:W]
        world = np.stack([xs * PX + lo[0], ys * PX + lo[1]], -1)
        src = (world - t) @ r / s  # r orthonormal: inverse is transpose
        ci = ((src - origin) / lba1.BRICK_XZ).astype(int)
        inside = (ci[..., 0] >= 0) & (ci[..., 0] < rgb.shape[1]) & (ci[..., 1] >= 0) & (ci[..., 1] < rgb.shape[0])
        cx, cz = np.clip(ci[..., 0], 0, rgb.shape[1] - 1), np.clip(ci[..., 1], 0, rgb.shape[0] - 1)
        drawn = inside & (height[cz, cx] > -999)
        plan = np.zeros((H, W, 3)); plan[drawn] = rgb[cz, cx][drawn]

        left = base(); dl = ImageDraw.Draw(left)
        draw_marks(dl, [d2[b] for a, b, _, _ in pairs], to_px, (255, 60, 60), [n for _, _, n, _ in pairs])
        mid = Image.new("RGB", (W, H), (20, 20, 24)); mid.paste(Image.fromarray(plan.astype(np.uint8)), mask=Image.fromarray((drawn * 255).astype(np.uint8)))
        dm = ImageDraw.Draw(mid)
        fitted = lambda p: s * r @ np.asarray(p)[[0, 2]] + t
        draw_marks(dm, [fitted(d1[a]) for a, _, _, _ in pairs], to_px, (255, 220, 40), [n for _, _, n, _ in pairs])
        for n, o in offsets.items():
            c = [fitted(o + np.array([u, 0, v])) for u, v in ((0, 0), (32767, 0), (32767, 32767), (0, 32767))]
            dm.polygon([to_px(p) for p in c], outline=(255, 255, 255))
            dm.text(to_px(fitted(o + np.array([800, 0, 800]))), f"{n} {lba1.NAMES.get(n, '')}", fill=(255, 255, 255),
                    stroke_width=2, stroke_fill=(0, 0, 0))
        right = base()
        blend = np.asarray(right).astype(float); a = drawn[..., None] * 0.6
        right = Image.fromarray((blend * (1 - a) + plan * a).astype(np.uint8)); dr = ImageDraw.Draw(right)
        right_coast = [to_px(p) for p in coast]; dr.line(right_coast + right_coast[:1], fill=(255, 255, 255), width=2)
        for a1, b, _, _ in pairs:
            p1, p2 = to_px(fitted(d1[a1])), to_px(d2[b])
            dr.line([p1, p2], fill=(255, 255, 255), width=2)
        draw_marks(dr, [fitted(d1[a]) for a, _, _, _ in pairs], to_px, (255, 220, 40))
        draw_marks(dr, [d2[b] for _, b, _, _ in pairs], to_px, (255, 60, 60))
        sheet = Image.new("RGB", (W * 3 + 20, H + 30), (12, 12, 14))
        for i, (img, title) in enumerate([(left, "LBA2 coast, cubes, doors (red)"), (mid, "LBA1 scenes, doors (yellow)"),
                                          (right, f"LBA1 over LBA2 ({cfg.get('model', 'similarity')}): scale {s:.2f}, turn {turn:.1f} deg" if models else
                                           "LBA1 over LBA2: offset only, one shared point")]):
            sheet.paste(img, (i * (W + 10), 30)); ImageDraw.Draw(sheet).text((i * (W + 10) + 4, 8), title, fill=(230, 230, 230))
        sheet.save(os.path.join(OUT, f"{island}.png"))
        report[island] = dict(scale=s, turn_deg=turn, offset=t.tolist(), models=models, doors=res,
                              cover={n: dict(lba1=b, fraction=f) for n, (b, f) in cover.items()}, lba1_land_m2=area1,
                              lba2_land_m2=area2 / UNITS_PER_M ** 2,
                              lba1_scenes={n: o.tolist() for n, o in offsets.items()})
    json.dump(report, open(os.path.join(OUT, "rosetta.json"), "w"), indent=1)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
