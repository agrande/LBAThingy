"""LBA2's island decor bodies (*.OBL) drawn iso on contact sheets, to pick models for LBA1's town pieces (Phase 3 of
LBA1_ISLANDS_PLAN.md, Resources/Models/lba1_kit.json).

Usage: lba2_decor_sheet.py [island ...]   (default CITADEL CITABAU DESERT OTRINGAL)
Writes Saved/LookDev/lba1_kit/decor_<ISLAND>.png: each body with its index and size in meters (x, y up, z). Flat shaded
from palette colors, a textured polygon drawn in its own color (tan): enough to recognize them, not their in-game colors.
Some bodies sit far above their pivot (CITADEL 2: 2.25 m), which the kit's "fit" corrects.
"""
import os, struct, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = os.path.join(PROJECT, "Saved/LookDev/lba1_kit")
GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2").rstrip("/\\") + "/"
UNIT_M = 1 / 800.0  # LBA2 units to meters (512 units = 64 cm)


def count(path):
    d = open(path, "rb").read(4)
    return struct.unpack_from("<I", d)[0] // 4


def body(data):
    """Points (rest positions) and polys [(vertices, color)] of an LBA2 body (Formats/LBABody.cpp)."""
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]
    s16 = lambda o: struct.unpack_from("<h", data, o)[0]
    s32 = lambda o: struct.unpack_from("<i", data, o)[0]
    ng, og = s32(0x20), s32(0x24)
    npt, opt = s32(0x28), s32(0x2C)
    opl, oli = s32(0x44), s32(0x4C)
    groups = [(u16(og + i * 8), u16(og + i * 8 + 2), u16(og + i * 8 + 4)) for i in range(ng)]
    raw = [(s16(opt + i * 8), s16(opt + i * 8 + 2), s16(opt + i * 8 + 4), u16(opt + i * 8 + 6)) for i in range(npt)]
    origins = [np.zeros(3)] * ng
    for i, (parent, pivot, _) in enumerate(groups):
        if parent != 0xFFFF and parent < i and pivot < len(raw):
            origins[i] = origins[parent] + np.array(raw[pivot][:3], float)
    pts = np.array([np.array(p[:3], float) + (origins[p[3]] if p[3] < ng else 0) for p in raw]) if raw else np.zeros((0, 3))
    polys = []
    at = opl; end = oli if oli > opl else len(data)
    while at + 8 <= end:
        tw, n, size = u16(at), u16(at + 2), struct.unpack_from("<I", data, at + 4)[0]
        if size <= 8 or n == 0:
            break
        block = (size - 8) // n; quad = tw & 0x8000
        for k in range(n):
            p = at + 8 + k * block
            vs = [u16(p + v * 2) for v in range(4 if quad else 3)]
            if all(v < len(pts) for v in vs):
                polys.append((vs, u16(p + 8) & 0xFF))
        at += size
    return pts, polys


def draw(pts, polys, pal, px=900.0):
    """Iso view (camera from +x, +z, above), painter's order, Lambert from the upper left. px = units per 100 pixels."""
    if not len(pts) or not polys:
        return None
    c, s = np.cos(np.radians(45)), np.sin(np.radians(45))
    # LBA x right, y up, z towards the viewer's left as in the iso games.
    sx = (pts[:, 0] - pts[:, 2]) * c
    depth = (pts[:, 0] + pts[:, 2]) * s
    sy = -pts[:, 1] * 0.82 + depth * 0.5
    scale = 100.0 / px
    x0, y0 = sx.min(), sy.min()
    w = int((sx.max() - x0) * scale) + 4; h = int((sy.max() - y0) * scale) + 4
    if w > 2000 or h > 2000:
        return draw(pts, polys, pal, px * max(w, h) / 1000.0)
    img = Image.new("RGBA", (max(w, 4), max(h, 4)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    light = np.array([-0.4, 0.8, 0.45]); light /= np.linalg.norm(light)
    order = sorted(polys, key=lambda pc: np.mean(depth[pc[0]]))
    for vs, col in order:
        p = pts[vs]
        nrm = np.cross(p[1] - p[0], p[2] - p[0])
        ln = np.linalg.norm(nrm)
        shade = 0.55 + 0.45 * abs(nrm @ light) / ln if ln > 0 else 0.8
        rgb = tuple(int(min(255, v * shade)) for v in pal[col])
        d.polygon([((sx[v] - x0) * scale + 2, (sy[v] - y0) * scale + 2) for v in vs], fill=rgb + (255,))
    return img


def main():
    islands = sys.argv[1:] or ["CITADEL", "CITABAU", "DESERT", "OTRINGAL"]
    os.makedirs(OUT, exist_ok=True)
    pal = np.frombuffer(hqr.entry(GAME + "RESS.HQR", 0)[:768], np.uint8).reshape(256, 3)
    for island in islands:
        path = GAME + island + ".OBL"
        items = []
        for i in range(count(path)):
            try:
                data = hqr.entry(path, i)
            except Exception:
                continue
            if len(data) < 0x60:
                continue
            pts, polys = body(data)
            img = draw(pts, polys, pal)
            if img is None:
                continue
            size = (pts.max(0) - pts.min(0)) * UNIT_M
            items.append((img, "%d  %.1fx%.1fx%.1f" % (i, size[0], size[1], size[2])))
        cell = (200, 200); columns = 10
        rows = (len(items) + columns - 1) // columns
        out = Image.new("RGB", (columns * cell[0], rows * cell[1]), (40, 40, 48))
        dr = ImageDraw.Draw(out)
        for k, (image, label) in enumerate(items):
            at = (k % columns * cell[0], k // columns * cell[1])
            if image.width > cell[0] - 4 or image.height > cell[1] - 18:
                r = min((cell[0] - 4) / image.width, (cell[1] - 18) / image.height)
                image = image.resize((max(1, int(image.width * r)), max(1, int(image.height * r))), Image.LANCZOS)
            out.paste(image, (at[0] + 2, at[1] + 16), image)
            dr.text((at[0] + 2, at[1] + 2), label, fill=(255, 255, 255))
        out.save(os.path.join(OUT, "decor_%s.png" % island))
        print(island, len(items), "bodies")


if __name__ == "__main__":
    main()
