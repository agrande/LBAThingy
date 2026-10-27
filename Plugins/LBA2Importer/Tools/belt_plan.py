"""Plan of the Hamalayi belt on Twinsun's flat map (globe mode), from the original holomap.

HOLOMAP.HQR: entry 1 = Twinsun's altitudes (17 latitudes x 32 longitudes, 11.25 degrees apart, x2 on a radius of
1000), entry 2 = its texture (256x256, equirectangular: U = longitude, V = latitude from -90 to 90), RESS.HQR entry 0
= the palette. Writes Saved/LookDev/belt: the texture laid on the azimuthal map around the pole of Citadel's
hemisphere (as FLBAPlanetIsland::GetMapLocation), the belt's edges and the holomap's crest heights, and a profile.
The profile is the first, tall proposal (2026-10-27); the belt as built is lower and a third as wide (LBAPlanetBelt).
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr  # noqa: E402

GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../Saved/LookDev/belt")
RADIUS = 400.0  # m
ISLANDS = {"CITABAU": (53.5, 218.8), "DESERT": (84.2, 350.4)}


def load():
    pal = np.frombuffer(hqr.entry(os.path.join(GAME, "RESS.HQR"), 0), np.uint8).reshape(256, 3)
    tex = np.frombuffer(hqr.entry(os.path.join(GAME, "HOLOMAP.HQR"), 2), np.uint8).reshape(256, 256)
    alt = np.frombuffer(hqr.entry(os.path.join(GAME, "HOLOMAP.HQR"), 1), np.uint8).reshape(17, 32)
    return pal[tex].astype(int), alt


def lat_of_row(v):
    return (v + 0.5) / 256.0 * 180.0 - 90.0


def snow(rgb):
    """The belt's snow, ice and rock: light blues and whites, browns."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return (r + g + b > 300) | ((r > 70) & (r > b))


def not_sea(rgb):
    """Anything but the dark sea: the snow plus the mid-blue flanks around it (sloping down to the sea in the holomap
    mesh: altitudes at latitude 0, none at 11.25)."""
    return rgb.sum(axis=-1) > 150


def walk(mask, u, start, step, stop):
    v = start
    while 0 <= v + step < 256 and mask[v + step, u] and lat_of_row(v + step) * step < stop * step:
        v += step
    return lat_of_row(v)


def edges(rgb):
    """Per texture column, from the belt's whitest row (latitudes -15 to 2): the shore facing the pole, where the snow
    starts on that side, and the far edge (latitudes)."""
    land, white = not_sea(rgb), snow(rgb)
    shore, snowline, outer = [], [], []
    for u in range(256):
        rows = [v for v in range(256) if -15.0 <= lat_of_row(v) <= 2.0]
        core = max(rows, key=lambda v: rgb[v, u].sum())
        shore.append(walk(land, u, core, 1, 14.0))
        snowline.append(walk(white, u, core, 1, 14.0))
        outer.append(walk(land, u, core, -1, -40.0))
    return np.array(shore), np.array(snowline), np.array(outer)


def crest(alt):
    """Per altitude column (11.25 degrees of longitude): the belt's top (m on the planet), the latitudes 0 and -11.25."""
    return np.maximum(alt[8], alt[7]) * 2.0 / 1000.0 * RADIUS


def to_map(lat, lon):
    rho = RADIUS * math.radians(90.0 - lat)
    return rho * math.cos(math.radians(lon)), rho * math.sin(math.radians(lon))


def main():
    os.makedirs(OUT, exist_ok=True)
    rgb, alt = load()
    shore, snowline, outer = edges(rgb)
    tops = crest(alt)
    lons = (np.arange(256) + 0.5) / 256 * 360

    # The texture on the map: each pixel's distance from the pole and angle give its latitude and longitude.
    size, extent = 1000, 1600.0  # px, m from the pole to the image's edge
    scale = size / (2 * extent)
    ys, xs = np.mgrid[0:size, 0:size]
    mx = (xs + 0.5) / scale - extent
    my = extent - (ys + 0.5) / scale
    rho = np.hypot(mx, my)
    lat = 90.0 - np.degrees(rho / RADIUS)
    lon = np.degrees(np.arctan2(my, mx)) % 360.0
    u = np.clip((lon / 360.0 * 256).astype(int), 0, 255)
    v = np.clip(((lat + 90.0) / 180.0 * 256).astype(int), 0, 255)
    img = (rgb[v, u] * 0.6).astype(np.uint8)
    im = Image.fromarray(img, "RGB")
    d = ImageDraw.Draw(im)

    def px(x, y):
        return (x + extent) * scale, (extent - y) * scale

    for lats, color in ((shore, (255, 80, 60)), (snowline, (80, 255, 255)), (outer, (255, 200, 0))):
        pts = [px(*to_map(lats[c], lons[c])) for c in range(256)]
        d.line(pts + pts[:1], fill=color, width=2)
    pts = [px(*to_map(0.0, a)) for a in range(0, 361, 2)]
    d.line(pts, fill=(255, 255, 255), width=1)
    for c, top in enumerate(tops):
        x, y = px(*to_map(-4.0, c * 11.25))
        d.text((x - 8, y - 6), "%d" % round(top), fill=(255, 255, 255))
    for name, (la, lo) in ISLANDS.items():
        x, y = px(*to_map(la, lo))
        d.ellipse((x - 5, y - 5, x + 5, y + 5), outline=(255, 0, 255), width=2)
        d.text((x + 8, y - 6), name, fill=(255, 0, 255))
    d.text((10, 10), "Twinsun flat map (pole at the center), R %d m, 1 px = %.1f m, holomap texture dimmed" % (RADIUS, 1 / scale), fill=(255, 255, 255))
    d.text((10, 24), "red: shore facing the islands   cyan: foot of the wall (snow starts)   yellow: far edge   white: equator", fill=(255, 255, 255))
    d.text((10, 38), "numbers: crest height (m), the holomap's altitudes (x2 / 1000 of the radius) every 11.25 degrees", fill=(255, 255, 255))
    im.save(os.path.join(OUT, "belt_map.png"))

    def m_from_pole(la):
        return RADIUS * math.radians(90 - la)

    for name, (la, lo) in ISLANDS.items():
        ix, iy = to_map(la, lo)
        dist = [math.hypot(*(np.array(to_map(shore[c], lons[c])) - (ix, iy))) for c in range(256)]
        c = int(np.argmin(dist))
        print("%s: nearest shore %.0f m away (longitude %.0f); island %.0f m from the pole" % (name, dist[c], lons[c], m_from_pole(la)))
    for label, lats in (("shore", shore), ("snowline", snowline), ("far edge", outer)):
        print("%s latitude: min %.1f max %.1f mean %.1f -> %.0f..%.0f m from the pole" % (
            label, lats.min(), lats.max(), lats.mean(), m_from_pole(lats.max()), m_from_pole(lats.min())))
    print("flank (shore to snowline) m on the map: min %.0f max %.0f mean %.0f" % tuple(
        f(RADIUS * np.radians(shore - snowline)) for f in (np.min, np.max, np.mean)))
    print("crest m:", " ".join("%d" % round(t) for t in tops), "min", round(tops.min()), "mean", round(tops.mean()))
    print("map circumference at the shore %.0f m (on the planet %.0f m)" % (
        2 * math.pi * m_from_pole(shore.mean()), 2 * math.pi * RADIUS * math.cos(math.radians(shore.mean()))))
    profile()


# Proposed cross-section (m): a walkable flank from the shore, a cliff band no one walks up, snow slopes to the crest.
FLANK_TOP, CLIFF_TOP, CLIFF_RUN, MIN_CREST = 20.0, 50.0, 12.0, 70.0
DINOFLY_CEILINGS = ((29.0, "Dinofly max, 1 beat/s"), (41.0, "3 beats/s"))


def section(s, flank, crest_height, crest_at, far):
    """Height at s m from the shore."""
    if s < 0:
        return -8.0 * min(-s / 30.0, 1.0)
    if s < flank:
        return FLANK_TOP * (s / flank) ** 1.6
    if s < flank + CLIFF_RUN:
        return FLANK_TOP + (CLIFF_TOP - FLANK_TOP) * (s - flank) / CLIFF_RUN
    if s < crest_at:
        t = (s - flank - CLIFF_RUN) / (crest_at - flank - CLIFF_RUN)
        return CLIFF_TOP + (crest_height - CLIFF_TOP) * math.sin(t * math.pi / 2)
    if s < far:
        t = (s - crest_at) / (far - crest_at)
        return crest_height * math.cos(t * math.pi / 2) ** 0.7
    return -8.0


def profile():
    w, h, sx, sy, x0, y0 = 1000, 420, 2.4, 2.0, 40, 360
    im = Image.new("RGB", (w, h), (18, 24, 34))
    d = ImageDraw.Draw(im)

    def p(s, z):
        return x0 + s * sx, y0 - z * sy

    d.line([p(-15, 0), p(390, 0)], fill=(40, 90, 140), width=1)
    for z, label in DINOFLY_CEILINGS:
        d.line([p(-15, z), p(390, z)], fill=(255, 200, 0), width=1)
        d.text(p(338, z + 6), label + ": %d m" % z, fill=(255, 200, 0))
    cases = (("crest 70 m (the minimum; holomap low 56)", MIN_CREST, (120, 200, 255)),
             ("crest 153 m (holomap high)", 153.0, (230, 235, 250)))
    for i, (label, top, color) in enumerate(cases):
        flank, crest_at, far = 80.0, 170.0, 330.0
        pts = [p(s, section(s, flank, top, crest_at, far)) for s in np.arange(-15, 390, 1.0)]
        d.line(pts, fill=color, width=2)
        d.text((x0 + 10, 60 + 14 * i), label, fill=color)
    d.text(p(5, 30), "flank: walkable,\nDinofly lands", fill=(200, 200, 200))
    d.text(p(80 + CLIFF_RUN + 2, 33), "cliff band 20-50 m, %d deg:\nsteeper than Twinsen walks (55)" % round(
        math.degrees(math.atan((CLIFF_TOP - FLANK_TOP) / CLIFF_RUN))), fill=(200, 200, 200))
    d.text(p(178, 150), "crest", fill=(200, 200, 200))
    d.text(p(222, 95), "far side (other hemisphere):\nnever reached, never seen", fill=(150, 150, 150))
    d.text(p(-14, -12), "sea / shore", fill=(150, 150, 150))
    d.text((10, 10), "Belt cross-section, from the shore facing the islands (m on the planet; sideways x1.2 of height)", fill=(255, 255, 255))
    d.text((10, 24), "shore -> wall foot: 80 m typical (texture: 18 to 220 m; kept 30 to 150), wall foot -> crest ~90 m, crest -> far shore ~160 m", fill=(255, 255, 255))
    im.save(os.path.join(OUT, "belt_profile.png"))


if __name__ == "__main__":
    main()
