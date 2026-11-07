"""What LBA1's towns on the stub islands are made of: the recurring pieces (layouts) and the ground tiles.

Usage: lba1_kit_survey.py [out dir]   (default Saved/LookDev/lba1_kit)

A town scene's grid places layouts from that scene's own library (BLL entry n), so the same piece has a different
index in each scene: pieces are grouped across scenes by their bricks. Writes:
  pieces.png / pieces.json   the pieces by instances placed, drawn iso, with size and scenes
  ground_<island>.png        each island's most covered top bricks (a tile's share of the visible ground)
  ground.json                the same with each tile's mean color, palette ramps and ground-likeness
  palettes.png               LBA1's palette against LBA2's (Citabau's) as 16 x 16 ramps
"""
import collections, json, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hqr
import lba1

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(TOOLS, "../../.."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "Saved/LookDev/lba1_kit")
LBA2_GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2").rstrip("/\\") + "/"
ISLANDS = {
    "Principal": [11, 12, 13, 17, 18, 19, 25, 24, 37],
    "Proxima": [42, 44, 45, 46, 47],
    "Rebellion": [59],
}


def indexed_brick(index):
    """Palette indices of a brick sprite, 38 x 48, -1 = transparent."""
    d = hqr.entry(lba1.GAME + "LBA_BRK.HQR", index - 1)
    img = np.full((38, 48), -1, np.int32)
    lines, ox, oy = d[1], d[2], d[3]; at = 4
    for line in range(lines):
        runs = d[at]; at += 1; x = 0
        for _ in range(runs):
            spec = d[at]; at += 1; n = (spec & 63) + 1; t = spec >> 6
            for k in range(n):
                c = d[at] if t == 2 else d[at + k] if t in (1, 3) else 0
                if t and 0 <= x + ox < 48 and 0 <= line + oy < 38:
                    img[line + oy, x + ox] = c
                x += 1
            at += 1 if t == 2 else n if t in (1, 3) else 0
    return img


def draw_piece(layout, scale=1):
    """A layout's bricks composed as the iso view draws them."""
    sx, sy, sz = layout["size"]
    w = (sx + sz) * 24 + 24; h = (sx + sz) * 12 + sy * 15 + 38
    canvas = np.zeros((h, w, 4), np.uint8)
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                _, _, br = layout["blocks"][x + z * sx + y * sx * sz]
                if not br:
                    continue
                img = lba1.brick(br)
                px = (sz - 1 - z + x) * 24
                py = (x + z) * 12 + (sy - 1 - y) * 15
                region = canvas[py:py + 38, px:px + 48]
                mask = img[..., 3] > 0
                region[mask] = img[mask]
    return Image.fromarray(canvas, "RGBA")


def main():
    os.makedirs(OUT, exist_ok=True)
    pieces = {}
    top_counts = {name: collections.Counter() for name in ISLANDS}
    for island, scenes in ISLANDS.items():
        for n in scenes:
            grid = lba1.load_grid(n); lib = lba1.load_library(n)
            # Instances: a layout's first block (block 0) placed in the grid.
            xs, ys, zs = np.nonzero(grid)
            for x, y, z in zip(xs, ys, zs):
                v = grid[x, y, z]
                layout, block = v >> 8, v & 255
                if layout < 1 or layout > len(lib) or block != 0:
                    continue
                entry = lib[layout - 1]
                key = (tuple(entry["size"]), tuple(b[2] for b in entry["blocks"]))
                # The example names it in Resources/Models/lba1_kit.json: scene and 0-based layout index, as the importer counts.
                p = pieces.setdefault(key, dict(layout=entry, count=0, scenes=collections.Counter(), islands=set(), example="%d:%d" % (n, layout - 1)))
                p["count"] += 1; p["scenes"][n] += 1; p["islands"].add(island)
            # The top brick of every column (the ground and the roofs as seen from above).
            for x in range(lba1.SIZE_X):
                for z in range(lba1.SIZE_Z):
                    for y in range(lba1.SIZE_Y - 1, -1, -1):
                        v = grid[x, y, z]
                        if not v:
                            continue
                        layout, block = v >> 8, v & 255
                        if 1 <= layout <= len(lib) and block < len(lib[layout - 1]["blocks"]):
                            shape, typ, br = lib[layout - 1]["blocks"][block]
                            if br and typ != lba1.WATER_BRICK:
                                top_counts[island][br] += 1
                                break

    # Pieces, most placed first.
    ordered = sorted(pieces.items(), key=lambda kv: -kv[1]["count"])
    total = sum(p["count"] for p in pieces.values())
    print("pieces: %d distinct, %d instances" % (len(pieces), total))
    for share in (0.5, 0.75, 0.9, 0.95):
        running = 0
        for i, (_, p) in enumerate(ordered):
            running += p["count"]
            if running >= share * total:
                print("  %d %% of the instances: %d pieces" % (share * 100, i + 1))
                break
    cells = sum(np.prod(k[0]) * p["count"] for k, p in ordered)
    big = [(k, p) for k, p in ordered if np.prod(k[0]) >= 8]
    print("  cells %d; pieces of 8+ cells %d (%d instances)" % (cells, len(big), sum(p["count"] for _, p in big)))
    summary = []
    for rank, (key, p) in enumerate(ordered):
        summary.append(dict(rank=rank, example=p["example"], size=key[0], count=p["count"], scenes=dict(p["scenes"]), islands=sorted(p["islands"]),
                            bricks=sorted(set(b for b in key[1] if b))))
    json.dump(summary, open(os.path.join(OUT, "pieces.json"), "w"), indent=0, default=int)

    def sheet(items, file, columns=10, cell=(190, 190)):
        rows = (len(items) + columns - 1) // columns
        out = Image.new("RGB", (columns * cell[0], rows * cell[1]), (40, 40, 48))
        draw = ImageDraw.Draw(out)
        for i, (image, label) in enumerate(items):
            at = (i % columns * cell[0], i // columns * cell[1])
            if image.width > cell[0] - 4 or image.height > cell[1] - 18:
                ratio = min((cell[0] - 4) / image.width, (cell[1] - 18) / image.height)
                image = image.resize((max(1, int(image.width * ratio)), max(1, int(image.height * ratio))), Image.NEAREST)
            out.paste(image, (at[0] + 2, at[1] + 16), image)
            draw.text((at[0] + 2, at[1] + 2), label, fill=(255, 255, 255))
        out.save(os.path.join(OUT, file))

    # Big pieces first page, small pieces second: a lamp or a tree is a tall column, a floor tile a single cell.
    sheet([(draw_piece(p["layout"]), "#%d x%d %s" % (i, p["count"], "x".join(map(str, k[0])))) for i, (k, p) in enumerate(ordered[:100])], "pieces.png")
    sized = sorted(((k, p) for k, p in ordered if k[0][1] >= 3 or k[0][0] * k[0][2] >= 4), key=lambda kv: -kv[1]["count"] * np.prod(kv[0][0]))
    sheet([(draw_piece(p["layout"]), "x%d %s  %s" % (p["count"], "x".join(map(str, k[0])), p["example"])) for k, p in sized[:150]], "pieces_big.png")

    # Ground tiles per island.
    ground = {}
    pal = lba1.palette().astype(np.float32)
    for island, counts in top_counts.items():
        total_top = sum(counts.values())
        items = []; rows = []
        for br, c in counts.most_common(60):
            idx = indexed_brick(br)
            ys, xs = np.mgrid[0:38, 0:48]
            top = (np.abs(xs - 23.5) / 24 + np.abs(ys - 11.5) / 12 <= 0.85) & (idx >= 0)
            values = idx[top]
            ramps = collections.Counter((values // 16).tolist())
            color = pal[values].mean(0) if values.size else np.zeros(3)
            rows.append(dict(brick=br, share=c / total_top, color=color.round().tolist(), ramps=ramps.most_common(3),
                             coverage=float(top.sum()) / max(1, ((np.abs(xs - 23.5) / 24 + np.abs(ys - 11.5) / 12) <= 0.85).sum())))
            img = Image.fromarray(lba1.brick(br), "RGBA").resize((96, 76), Image.NEAREST)
            items.append((img, "%d %.1f%% r%s" % (br, 100 * c / total_top, ",".join(str(r) for r, _ in ramps.most_common(2)))))
        sheet(items, "ground_%s.png" % island, columns=10, cell=(120, 100))
        ground[island] = rows
        print(island, "top bricks", len(counts), "top 20 cover %.0f %%" % (100 * sum(c for _, c in counts.most_common(20)) / total_top))
        for r in rows[:14]:
            print("  brick %4d %5.1f %%  color %s  ramps %s" % (r["brick"], 100 * r["share"], r["color"], r["ramps"]))
    json.dump(ground, open(os.path.join(OUT, "ground.json"), "w"), indent=0, default=int)

    # Palettes side by side.
    d = hqr.entry(LBA2_GAME + "RESS.HQR", 0)
    lba2_pal = np.frombuffer(d[:768], np.uint8).reshape(256, 3)
    lba1_pal = lba1.palette()
    size = 18
    out = Image.new("RGB", (size * 16 * 2 + 30, size * 16 + 20), (20, 20, 20))
    draw = ImageDraw.Draw(out)
    for name, p, ox in (("LBA1", lba1_pal, 0), ("LBA2 (RESS 0)", lba2_pal, size * 16 + 30)):
        draw.text((ox + 2, 2), name, fill=(255, 255, 255))
        for i in range(256):
            x = ox + (i % 16) * size; y = 20 + (i // 16) * size
            draw.rectangle((x, y, x + size - 1, y + size - 1), fill=tuple(int(v) for v in p[i]))
    out.save(os.path.join(OUT, "palettes.png"))
    same = int((np.abs(lba1_pal.astype(int) - lba2_pal.astype(int)).sum(1) <= 12).sum())
    print("palette entries within 12 (sum of RGB differences) of LBA2's:", same, "of 256")


if __name__ == "__main__":
    main()
