"""Prototype of how LBA1's ground tiles join LBA2's palette: pattern from LBA1, colors from LBA2.

Usage: lba1_recolor_proto.py [out dir]   (default Saved/LookDev/lba1_kit)

Each row is a ground kind: a 3 x 3 patch of LBA1's top bricks, unprojected to a square, shown
  raw      LBA1's own colors
  today    what the look shader makes of them: every pixel snapped to the nearest color of Citabau's palette
  proposed each pixel's brightness placed on the LBA2 ramp of that kind, the tile's median at the LBA2 kind's value
  variants the same on another island's ramp offset, to judge how far per-island variety can go
and next to them a crop of Citabau's own ground texture of that kind, the reference.
"""
import os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr
import lba1
import terrain_stats as ts

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/lba1_kit")
SQUARE = 24      # texels per tile side
LUMA = np.array([0.299, 0.587, 0.114])

# Kind: LBA1 bricks of a 3 x 3 patch, the LBA2 color it should reach (the stubs' measured medians), natural or not.
KINDS = [
    ("grass", list(range(588, 597)), (120, 140, 96), True),
    ("earth", list(range(550, 559)), (124, 86, 58), True),
    ("dark ground", list(range(964, 973)), (136, 126, 128), True),
    ("paving", [509, 510, 512, 513, 509, 512, 510, 513, 509], None, False),
]


def unproject(brick):
    """A brick's top diamond as a square of RGBA."""
    img = lba1.brick(brick)
    out = np.zeros((SQUARE, SQUARE, 4), np.uint8)
    for v in range(SQUARE):
        for u in range(SQUARE):
            a = (u + 0.5) / SQUARE; b = (v + 0.5) / SQUARE
            x = int(23.5 + (a - b) * 23.5); y = int(0.5 + (a + b) * 11.5)
            out[v, u] = img[min(y, 37), min(max(x, 0), 47)]
    return out


def patch(bricks):
    rows = []
    for j in range(3):
        rows.append(np.concatenate([unproject(bricks[j * 3 + i])[:, :, :3] for i in range(3)], axis=1))
    return np.concatenate(rows, axis=0).astype(np.float32)


def nearest(pixels, palette):
    d = ((pixels[..., None, :] - palette[1:]) ** 2 * np.array([0.3, 0.59, 0.11])).sum(-1)
    return palette[1:][d.argmin(-1)]


def ramp_of(color, palette):
    """The palette's 16-step ramp holding the color nearest to the target, and that step."""
    d = ((palette - np.array(color, np.float32)) ** 2 * np.array([0.3, 0.59, 0.11])).sum(-1)
    d[0] = 1e12
    i = int(d.argmin())
    return i // 16, i % 16


def on_ramp(pixels, palette, ramp, target_step, contrast=1.0):
    """Each pixel's brightness on the ramp: the tile's median on the target step."""
    colors = palette[ramp * 16:ramp * 16 + 16]
    steps = colors @ LUMA
    luma = pixels @ LUMA
    target = steps[target_step]
    wanted = target + (luma - np.median(luma)) * contrast * max(target, 1.0) / max(float(np.median(luma)), 1.0)
    # Nearest step in luminance (the ramps rise monotonically in the parts used by ground colors).
    index = np.abs(wanted[..., None] - steps).argmin(-1)
    return colors[index], index


def main():
    os.makedirs(OUT, exist_ok=True)
    pal = ts.palette("CITABAU")
    tex = np.frombuffer(hqr.entry(ts.GAME + "CITABAU.ILE", 1)[:65536], np.uint8).reshape(256, 256)
    tex_rgb = pal[tex]
    classes = ts.classify(tex_rgb)
    print("Citabau ground texture, per class: pixels, top ramps")
    for k, name in enumerate(ts.CLASSES):
        where = classes == k
        if where.sum() == 0:
            continue
        ramps, counts = np.unique(tex[where] // 16, return_counts=True)
        order = counts.argsort()[::-1][:4]
        print("  %-6s %6d  median %s  ramps %s" % (name, where.sum(), np.median(tex_rgb[where], axis=0).astype(int).tolist(),
                                                  [(int(ramps[o]), int(counts[o])) for o in order]))

    scale = 3
    side = SQUARE * 3 * scale
    columns = ["LBA1 raw", "today: nearest palette", "proposed: LBA2 ramp", "brighter step (+1)", "next ramp (variety)"]
    out = Image.new("RGB", ((side + 8) * len(columns) + 8, (side + 26) * len(KINDS) + 8), (32, 32, 38))
    draw = ImageDraw.Draw(out)
    for row, (name, bricks, target, natural) in enumerate(KINDS):
        raw = patch(bricks)
        if target is None:
            # Man-made: keep LBA1's hue. The ramp of its own median color, its median unchanged.
            target = tuple(np.median(raw.reshape(-1, 3), axis=0))
        ramp, step = ramp_of(target, pal)
        proposed, index = on_ramp(raw, pal, ramp, step)
        brighter, _ = on_ramp(raw, pal, ramp, min(step + 1, 15))
        other = ramp + 1 if ramp < 15 else ramp - 1
        variant, _ = on_ramp(raw, pal, other, step)
        print("%-12s target %s -> ramp %d step %d; steps used %s; raw median %s" % (name, tuple(int(v) for v in target), ramp, step,
              np.unique(index).tolist(), np.median(raw.reshape(-1, 3), axis=0).astype(int).tolist()))
        for col, picture in enumerate([raw, nearest(raw, pal), proposed, brighter, variant]):
            image = Image.fromarray(picture.astype(np.uint8)).resize((side, side), Image.NEAREST)
            x = 8 + col * (side + 8); y = 22 + row * (side + 26)
            out.paste(image, (x, y))
            draw.text((x, y - 16), "%s: %s" % (name, columns[col]), fill=(240, 240, 240))
    out.save(os.path.join(OUT, "recolor_proto.png"))
    Image.fromarray(tex_rgb.astype(np.uint8)).resize((768, 768), Image.NEAREST).save(os.path.join(OUT, "citabau_ground.png"))


if __name__ == "__main__":
    main()
