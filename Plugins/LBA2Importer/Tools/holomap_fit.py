"""Checks the globe's islands against the original holomap: its texture (HOLOMAP.HQR entry 2, equirectangular as
CoorMapp.HMM maps it: U = longitude, V = latitude from the far pole down to the islands' pole) laid on the planet's
flat map, each island's coastline where the planet builder put it, and the heading, scale and offset that best fit
its painting (overlap of its land with the painted land around its arrow), unmirrored and mirrored.

Input: Saved/LookDev/holomap/planet.json from dump_planet.py. Output there: map.png (the whole hemisphere, magenta =
placed coastlines, white = equator) and fit_<ISLAND>.png (yellow = painted land, magenta = placed, green = best fit).
Usage: python holomap_fit.py [ISLAND ...]
"""
import json
import math
import os
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr  # noqa: E402

GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../Saved/LookDev/holomap")


def load_texture():
    pal = np.frombuffer(hqr.entry(os.path.join(GAME, "RESS.HQR"), 0), np.uint8).reshape(256, 3)
    return pal[np.frombuffer(hqr.entry(os.path.join(GAME, "HOLOMAP.HQR"), 2), np.uint8).reshape(256, 256)].astype(int)


def map_location(radius, lat, lon):
    """FLBAPlanetIsland::GetMapLocation (m)."""
    rho = radius * math.radians(90.0 - lat)
    return np.array([rho * math.cos(math.radians(lon)), rho * math.sin(math.radians(lon))])


def map_yaw(lon, heading):
    """FLBAPlanetIsland::GetMapYaw (radians)."""
    lo, he = math.radians(lon), math.radians(heading)
    east, north = (-math.sin(lo), math.cos(lo)), (-math.cos(lo), -math.sin(lo))
    return math.atan2(east[1] * math.cos(he) + north[1] * math.sin(he), east[0] * math.cos(he) + north[0] * math.sin(he))


def heading_for_yaw(lon, yaw):
    return min(np.arange(-180.0, 180.0, 0.5), key=lambda h: abs((math.degrees(map_yaw(lon, h) - yaw) + 180.0) % 360.0 - 180.0))


def sample(tex, radius, mx, my):
    """The texture at map points (m): Unreal top-down, so images show +X up and +Y right."""
    lat = 90.0 - np.degrees(np.hypot(mx, my) / radius)
    lon = np.degrees(np.arctan2(my, mx)) % 360.0
    return tex[np.clip(((lat + 90.0) / 180.0 * 256).astype(int), 0, 255), np.clip((lon / 360.0 * 256).astype(int), 0, 255)]


def outline(mask):
    return mask & ~(np.array(Image.fromarray(mask.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(3))) > 0)


def whole_map(tex, planet):
    radius = planet["radius"] / 100.0
    size, extent = 1000, 1.75 * radius
    scale = size / (2 * extent)
    ys, xs = np.mgrid[0:size, 0:size]
    img = sample(tex, radius, extent - (ys + 0.5) / scale, (xs + 0.5) / scale - extent)
    im = Image.fromarray((img * 0.75).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(im)

    def px(p):
        return (p[1] + extent) * scale, (extent - p[0]) * scale

    for island in planet["islands"]:
        yaw = map_yaw(island["lon"], island["heading"])
        origin = map_location(radius, island["lat"], island["lon"])
        pts = [px(origin + rotate((np.array(p) - island["center"]) / 100.0, yaw)) for p in island["coast"]]
        d.line(pts + pts[:1], fill=(255, 0, 255), width=2)
        x, y = px(origin)
        d.text((x + 6, y - 14), island["name"], fill=(255, 255, 0))
    d.line([px(map_location(radius, 0.0, a)) for a in range(0, 361, 2)], fill=(255, 255, 255), width=1)
    d.text((8, 8), "holomap texture on the flat map, R %d m; magenta: placed coastlines, white: equator" % radius, fill=(255, 255, 255))
    im.save(os.path.join(OUT, "map.png"))


def rotate(p, yaw):
    c, s = math.cos(yaw), math.sin(yaw)
    return np.array([c * p[..., 0] - s * p[..., 1], s * p[..., 0] + c * p[..., 1]]).T


def fit(tex, planet, island):
    radius = planet["radius"] / 100.0
    half = 200
    n = 2 * half
    origin = map_location(radius, island["lat"], island["lon"])
    ys, xs = np.mgrid[0:n, 0:n]
    img = sample(tex, radius, origin[0] + (half - ys - 0.5), origin[1] + (xs + 0.5 - half))
    land = np.array(Image.fromarray(((img.sum(-1) > 150) * 255).astype(np.uint8)).filter(ImageFilter.MedianFilter(5))) > 127
    # The painted land touching the arrow.
    ring = sorted(((abs(y - half) + abs(x - half), y, x) for y in range(half - 40, half + 40) for x in range(half - 40, half + 40) if land[y, x]))
    paint = np.zeros_like(land)
    queue = deque([ring[0][1:]])
    paint[ring[0][1:]] = True
    while queue:
        y, x = queue.popleft()
        for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= ny < n and 0 <= nx < n and land[ny, nx] and not paint[ny, nx]:
                paint[ny, nx] = True
                queue.append((ny, nx))
    coast = (np.array(island["coast"]) - island["center"]) / 100.0

    def mask(yaw, mirror, scale, tx, ty):
        q = rotate(coast * [1.0, -1.0 if mirror else 1.0] * scale, yaw) + [tx, ty]
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).polygon([(half + y, half - x) for x, y in q], fill=1)
        return np.array(m, bool)

    def overlap(m):
        return (m & paint).sum() / max(1, (m | paint).sum())

    current = map_yaw(island["lon"], island["heading"])
    placed = mask(current, False, 1.0, 0, 0)
    print("%s: heading %.1f, overlap %.2f; painted land %d m2, ours %d m2" % (island["name"], island["heading"], overlap(placed), paint.sum(), placed.sum()))
    out = (img * 0.6).astype(np.uint8)
    out[outline(paint)] = (255, 255, 0)
    out[outline(placed)] = (255, 0, 255)
    for mirror in (False, True):
        best = (0.0,)
        for yaw in range(0, 360, 4):
            for scale in (0.7, 0.85, 1.0, 1.15, 1.3, 1.5):
                for tx in (-20, 0, 20):
                    for ty in (-20, 0, 20):
                        v = overlap(mask(math.radians(yaw), mirror, scale, tx, ty))
                        if v > best[0]:
                            best = (v, yaw, scale, tx, ty)
        _, yaw0, scale0, tx0, ty0 = best
        for yaw in range(yaw0 - 4, yaw0 + 5):
            for scale in np.arange(scale0 - 0.1, scale0 + 0.11, 0.05):
                for tx in range(tx0 - 15, tx0 + 16, 5):
                    for ty in range(ty0 - 15, ty0 + 16, 5):
                        v = overlap(mask(math.radians(yaw), mirror, scale, tx, ty))
                        if v > best[0]:
                            best = (v, yaw, scale, tx, ty)
        v, yaw, scale, tx, ty = best
        print("  %s: overlap %.2f at heading %.1f, scale %.2f, offset (%d, %d) m" % (
            "mirrored" if mirror else "best", v, heading_for_yaw(island["lon"], math.radians(yaw)), scale, tx, ty))
        if not mirror:
            out[outline(mask(math.radians(yaw), False, scale, tx, ty))] = (0, 255, 0)
    Image.fromarray(out).resize((2 * n, 2 * n), Image.NEAREST).save(os.path.join(OUT, "fit_%s.png" % island["name"]))


def main():
    with open(os.path.join(OUT, "planet.json")) as f:
        planet = json.load(f)
    tex = load_texture()
    whole_map(tex, planet)
    names = [a.upper() for a in sys.argv[1:]]
    for island in planet["islands"]:
        if not names or island["name"] in names:
            fit(tex, planet, island)


if __name__ == "__main__":
    main()
