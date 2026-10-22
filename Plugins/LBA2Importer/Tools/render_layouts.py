"""Renders layouts of a library (iso, original bricks) into one sheet.
Usage: render_layouts.py <lib> <layout,layout,...> <out.png>"""
import os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S
from hqr import entry

lib = int(sys.argv[1]); wanted = [int(v) for v in sys.argv[2].split(",")]; out = sys.argv[3]
L = S.load_library(lib)
ress = entry(S.GAME + "RESS.HQR", 0)
pal = [tuple(ress[i * 3:i * 3 + 3]) + (255,) for i in range(256)]

def sprite(brick):
    d = entry(S.BKG, S.BRICK_START + brick - 1)
    img = Image.new("RGBA", (48, 38)); px = img.load(); lines, ox, oy = d[1], d[2], d[3]; at = 4
    for line in range(lines):
        runs = d[at]; at += 1; x = 0
        for _ in range(runs):
            spec = d[at]; at += 1; n = (spec & 63) + 1; t = spec >> 6
            for k in range(n):
                c = d[at] if t == 2 else d[at + k] if t in (1, 3) else 0
                if t and 0 <= x + ox < 48 and 0 <= line + oy < 38:
                    px[x + ox, line + oy] = pal[c]
                x += 1
            at += 1 if t == 2 else n if t in (1, 3) else 0
    return img

tiles = []
for li in wanted:
    lo = L[li]; sx, sy, sz = lo["size"]
    w = max((sx + sz) * 24 + 8, 90); h = (sx + sz) * 12 + sy * 15 + 40
    img = Image.new("RGBA", (w, h), (40, 40, 48, 255))
    for x, y, z in sorted(((x, y, z) for x in range(sx) for y in range(sy) for z in range(sz)), key=lambda p: (p[0] + p[2], p[1])):
        br = lo["blocks"][z * sx * sy + y * sx + x][2]
        if br and br != S.FORBIDDEN:
            img.alpha_composite(sprite(br), ((x - z) * 24 + sz * 24 - 20, (x + z) * 12 - y * 15 + sy * 15))
    ImageDraw.Draw(img).text((2, 2), f"L{li} {sx}x{sy}x{sz}", fill=(255, 255, 0, 255))
    tiles.append(img)
cols = 8; cw = max(t.width for t in tiles); ch = max(t.height for t in tiles)
sheet = Image.new("RGBA", (cw * min(cols, len(tiles)), ch * ((len(tiles) + cols - 1) // cols)), (20, 20, 24, 255))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * cw, (i // cols) * ch))
sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
sheet.save(out)
