"""Reads LBA1 (Little Big Adventure) data: scenes, grids, block libraries, bricks. Used by lba1_rosetta.py.

Formats follow ScummVM's twine engine (scene.cpp loadSceneLBA1, grid.cpp, blocklibrary.cpp):
  scene n = SCENE.HQR entry n (LBA2 adds 1), its grid = LBA_GRI.HQR entry n, its library = LBA_BLL.HQR entry n
  grid = 64 x 64 u16 column offsets (index z * 64 + x), each column 25 cells of u16 (low byte layout 1-based,
         high byte block within the layout), then 32 bytes of library usage bits at the end
  cells 512 x 256 x 512 units (X, Y up, Z), as LBA2's; a cube change zone moves the hero to pos - mins + info
"""
import os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hqr import entry

# The game's data folder: LBA1_GAME, else the GOG install.
GAME = os.environ.get("LBA1_GAME", "C:/GOG Games/tlba-classic/Common").rstrip("/\\") + "/"
SIZE_X, SIZE_Y, SIZE_Z = 64, 25, 64
BRICK_XZ, BRICK_Y = 512, 256

# Text banks (scene byte 0) = island; ScummVM's TextBankId adds 3.
ISLANDS = ["Citadel", "Principal", "Desert", "Proxima", "Rebellion", "Hamalayi South", "Hamalayi North", "Tippet",
           "Brundle", "Fortress", "Polar"]

# ScummVM LBA1SceneId names for the southern islands' scenes (the rest are unnamed here).
NAMES = {0: "Prison", 1: "Outside the Citadel", 2: "Near the tavern", 3: "Near the pharmacy",
         4: "Near Twinsen's house", 5: "Twinsen's house", 6: "Harbour", 7: "Pharmacy", 8: "Temple of Bu 1",
         10: "Library", 11: "Principal harbour", 12: "Outside the fortress", 13: "Old Burg", 14: "Tavern",
         16: "Rabbibunny house", 17: "Ruins", 18: "Outside the library", 19: "Military camp",
         20: "Architect's house", 21: "Secret chamber", 22: "Ticket office", 23: "Prison",
         24: "Port Belooga", 25: "Peg Leg Street", 26: "Shop", 27: "Locksmith", 28: "Rabbibunny house",
         29: "Astronomer's house", 30: "Tavern", 31: "Astronomer's basement", 32: "Stables",
         33: "Tavern cellar", 34: "Sewer", 35: "Warehouse", 36: "Outside the Temple of Bu", 37: "Water tower",
         38: "Inside the water tower", 39: "Desert military camp", 40: "Temple of Bu 2", 41: "Temple of Bu 3",
         42: "Proxim City", 43: "Museum", 44: "Near the inventor's house", 45: "Upper rune stone",
         46: "Lower rune stone", 47: "Before the upper rune stone", 48: "Forger's house", 49: "Proxima prison",
         50: "Proxima shop", 51: "Proxima sewer", 52: "Peg Leg Street house", 53: "Grobo house",
         54: "Inventor's house", 55: "Sewer secret", 56: "Principal sewer secret", 57: "Desert maze",
         58: "House with the TV", 59: "Rebellion harbour", 60: "Rebel camp", 102: "Port Belooga house",
         104: "Ticket office", 105: "Inside the fortress", 116: "End sequence 1", 117: "End sequence 2",
         118: "Twinsen's house destroyed"}

ZONE_CUBE = 0


def parse_scene(n):
    d = entry(GAME + "SCENE.HQR", n)
    o = 0
    def g(fmt):
        nonlocal o
        v = struct.unpack_from("<" + fmt, d, o); o += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]
    s = {"index": n, "name": NAMES.get(n, f"scene {n}")}
    s["bank"], s["gameover"] = g("BB"); o += 4
    s["alpha"], s["beta"] = g("hh")
    o += 4 * 6 + 4
    s["music"] = g("B")
    s["hero"] = g("hhh")
    scripts = []  # track and life script bytes: hero's, then each actor's
    for _ in range(2):
        n = g("H"); scripts.append(d[o:o + n]); o += n
    actors = []
    for a in range(1, g("H")):
        flags, f3d, body, anim, spr = g("HHBBH"); pos = g("hhh")
        g("B"); g("H"); beta = g("h"); g("h"); g("H"); g("hhhh"); g("BBBB")
        for _ in range(2):
            n = g("H"); scripts.append(d[o:o + n]); o += n
        actors.append(dict(flags=flags, file3d=f3d, body=body, sprite=spr, pos=pos, beta=beta, life=scripts[-1]))
    zones = []
    for z in range(g("H")):
        mins = g("hhh"); maxs = g("hhh"); typ, num = g("Hh"); info = g("hhhh")
        zones.append(dict(mins=mins, maxs=maxs, type=typ, num=num, info=info))
    pts = [g("hhh") for _ in range(g("H"))]
    s["hero_life"] = scripts[1]
    s.update(island=ISLANDS[s["bank"]] if s["bank"] < len(ISLANDS) else f"bank {s['bank']}", actors=actors, zones=zones, points=pts)
    return s


def scene_count():
    d = open(GAME + "SCENE.HQR", "rb").read(4)
    return struct.unpack("<I", d)[0] // 4 - 1


def load_library(n):
    d = entry(GAME + "LBA_BLL.HQR", n)
    layouts = []
    for li in range(struct.unpack_from("<I", d, 0)[0] // 4):
        at = struct.unpack_from("<I", d, li * 4)[0]
        sx, sy, sz = d[at:at + 3]
        blocks = [struct.unpack_from("<BBH", d, at + 3 + b * 4) for b in range(sx * sy * sz)]
        layouts.append(dict(size=(sx, sy, sz), blocks=blocks))  # block = (shape, type, brick 1-based)
    return layouts


def load_grid(n):
    """Cells as an int array [x, y, z] of (layout << 8 | block); 0 = empty."""
    d = entry(GAME + "LBA_GRI.HQR", n)
    cube = np.zeros((SIZE_X, SIZE_Y, SIZE_Z), np.int32)
    for z in range(SIZE_Z):
        for x in range(SIZE_X):
            at = struct.unpack_from("<H", d, 2 * (z * SIZE_X + x))[0]
            runs = d[at]; at += 1; y = 0
            for _ in range(runs):
                flag = d[at]; at += 1; count = (flag & 63) + 1; typ = flag >> 6
                for k in range(count):
                    if typ == 1:
                        v = struct.unpack_from("<H", d, at); at += 2; v = v[0]
                    elif typ >= 2:
                        v = struct.unpack_from("<H", d, at)[0]
                    else:
                        v = 0
                    if y < SIZE_Y:
                        cube[x, y, z] = (v & 255) << 8 | v >> 8
                    y += 1
                if typ >= 2:
                    at += 2
    return cube


def palette():
    p = entry(GAME + "RESS.HQR", 0)
    return np.frombuffer(p[:768], np.uint8).reshape(256, 3)


_bricks = {}


def brick(index):
    """Brick sprite (1-based index) as an RGBA array 38 x 48, decoded like render_layouts.py's LBA2 bricks."""
    if index in _bricks:
        return _bricks[index]
    pal = palette()
    d = entry(GAME + "LBA_BRK.HQR", index - 1)
    img = np.zeros((38, 48, 4), np.uint8)
    lines, ox, oy = d[1], d[2], d[3]; at = 4
    for line in range(lines):
        runs = d[at]; at += 1; x = 0
        for _ in range(runs):
            spec = d[at]; at += 1; n = (spec & 63) + 1; t = spec >> 6
            for k in range(n):
                c = d[at] if t == 2 else d[at + k] if t in (1, 3) else 0
                if t and 0 <= x + ox < 48 and 0 <= line + oy < 38:
                    img[line + oy, x + ox, :3] = pal[c]; img[line + oy, x + ox, 3] = 255
                x += 1
            at += 1 if t == 2 else n if t in (1, 3) else 0
    _bricks[index] = img
    return img


def top_color(index):
    """Mean color of a brick's top face (the upper diamond of its iso sprite)."""
    img = brick(index)
    ys, xs = np.mgrid[0:38, 0:48]
    top = (np.abs(xs - 23.5) / 24 + np.abs(ys - 11.5) / 12 <= 0.8) & (img[..., 3] > 0)
    if not top.any():
        top = img[..., 3] > 0
    return img[top][:, :3].mean(0) if top.any() else np.array([0, 0, 0])


WATER_BRICK = 0xF1


def plan(n):
    """Top view of scene n's grid, all [z, x]: height (cells, -1 = empty), RGB of each column's top brick, and
    whether that brick is water."""
    cube = load_grid(n); lib = load_library(n)
    height = np.full((SIZE_Z, SIZE_X), -1, np.int32); rgb = np.zeros((SIZE_Z, SIZE_X, 3))
    water = np.zeros((SIZE_Z, SIZE_X), bool)
    cache = {}
    for x in range(SIZE_X):
        for z in range(SIZE_Z):
            for y in range(SIZE_Y - 1, -1, -1):
                v = cube[x, y, z]
                if not v:
                    continue
                layout, block = v >> 8, v & 255
                if layout < 1 or layout > len(lib) or block >= len(lib[layout - 1]["blocks"]):
                    continue
                shape, typ, br = lib[layout - 1]["blocks"][block]
                if not br:
                    continue
                if br not in cache:
                    cache[br] = top_color(br)
                height[z, x] = y; rgb[z, x] = cache[br]; water[z, x] = typ == WATER_BRICK
                break
    return height, rgb, water
