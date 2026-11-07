"""Prototype of the globe sea's regional colors (LBAIslandImporter's sea region atlas): the island's sea texture with
each palette color moved, by brightness rank, toward the oasis water (turquoise, Desert's Oasis scene) or the
holomap's ice fringe (near the Hamalayi), in 4 steps, each snapped to the island's palette. Writes a preview sheet."""
import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
import hqr

GAME = "C:/GOG Games/Little Big Adventure 2/"
OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# Measured in lba2cc's Oasis shot (refs/lba2cc-bin/shots/classic_55.png): the river's palette colors and their counts.
# Its greens down to 40 %: shallows brighten the sea's shade rows, and the greens' bright rows are neon; its highlight
# (96, 200, 200) is left to the shallows.
OASIS = [((36, 116, 100), 404), ((52, 132, 112), 654), ((56, 144, 120), 1089), ((44, 144, 152), 4420),
         ((64, 164, 136), 696), ((68, 172, 176), 2092)]
# The holomap's fringe along the Hamalayi (RESS palette 0 bank 12 and around): pale blues and whites.
# The palette's own sea bank paling to white: the holomap's fringe is light blue (159,199,227) and white (187,223,227).
ICE = [((128, 180, 212), 20), ((156, 196, 224), 35), ((188, 216, 236), 35), ((252, 252, 252), 10)]
STEPS = 4

def xpl_palette(entry):
    d = hqr.entry(GAME + "RESS.HQR", entry)
    off = int.from_bytes(d[4:8], "little")
    return np.frombuffer(d[off:off + 768], np.uint8).reshape(256, 3).astype(float)

def luma(c):
    lin = (np.asarray(c, float) / 255.0) ** 2.2
    return lin @ [0.2126, 0.7152, 0.0722]

def nearest(pal, c):
    w = np.array([2.0, 4.0, 3.0])
    d = (((pal[1:] - np.asarray(c, float)) ** 2) * w).sum(1)
    return 1 + int(np.argmin(d))

def remaps(pal, sea):
    idx, counts = np.unique(sea, return_counts=True)
    order = np.argsort([luma(pal[i]) for i in idx])
    idx, counts = idx[order], counts[order]
    q = (np.cumsum(counts) - counts / 2) / counts.sum()
    out = {}
    for name, ramp in (("ice", ICE), ("oasis", OASIS)):
        ramp = sorted(ramp, key=lambda e: luma(e[0]))
        rw = np.cumsum([e[1] for e in ramp]) / sum(e[1] for e in ramp)
        tables = []
        targets = {i: nearest(pal, ramp[int(np.searchsorted(rw, qi))][0]) for i, qi in zip(idx, q)}
        # Ice walks up a chain of the sea's colors and the ramp's, by brightness, so each step keeps the pattern;
        # turquoise is another hue: blended in linear light, then snapped.
        # The chain: the sea's main bank of 16 palette colors (others, like a teal, would tint the steps) and the targets.
        bank = idx[np.argmax(counts)] // 16
        chain = sorted({i for i in idx.tolist() if i // 16 == bank} | set(targets.values()), key=lambda i: luma(pal[i]))
        lumas = [luma(pal[c]) for c in chain]
        for step in range(1, STEPS + 1):
            f = step / STEPS
            t = np.arange(256)
            for i in idx:
                if name == "ice":
                    a = chain.index(i) if i in chain else min(int(np.searchsorted(lumas, luma(pal[i]))), len(chain) - 1)
                    b = chain.index(targets[i])
                    t[i] = chain[int(round(a + (b - a) * f))]
                else:
                    a = (pal[i] / 255) ** 2.2; b = (pal[targets[i]] / 255) ** 2.2
                    t[i] = nearest(pal, 255 * ((a + (b - a) * f) ** (1 / 2.2)))
            tables.append(t)
        out[name] = tables
    return out

for island, xpl, skysea in (("CITABAU", 42, 26), ("DESERT", 29, 13)):
    pal = xpl_palette(xpl)
    page = np.frombuffer(hqr.entry(GAME + "RESS.HQR", skysea), np.uint8).reshape(-1, 256)[:128, :128]
    tables = remaps(pal, page)
    rows = [pal[page]]
    for name in ("ice", "oasis"):
        rows.append(np.concatenate([pal[t[page]] for t in tables[name]], 1))
    first = np.concatenate([rows[0]] + [np.zeros_like(rows[0])] * (STEPS - 1), 1)
    sheet = np.concatenate([first, rows[1], rows[2]], 0).astype(np.uint8)
    Image.fromarray(sheet).resize((sheet.shape[1] * 2, sheet.shape[0] * 2), Image.NEAREST).save(os.path.join(OUT, f"{island}_sea_regions.png"))
    used = np.unique(page)
    print(island, "sea colors", [tuple(int(v) for v in pal[i]) for i in used][:20], len(used))
    for name in ("ice", "oasis"):
        last = tables[name][-1]
        print(" ", name, sorted({tuple(int(v) for v in pal[last[i]]) for i in used}))
