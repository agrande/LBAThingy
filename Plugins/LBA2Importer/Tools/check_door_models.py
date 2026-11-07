"""
Compares the generated door models (Tools/make_door_models.py) with their sprites in the game's isometric projection.

    python Plugins/LBA2Importer/Tools/check_door_models.py <out.png>

For each door: the sprite, the model drawn with the same projection and origin, and where their silhouettes differ
(white = both, red = sprite only, blue = model only), with the silhouettes' overlap (intersection over union).
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from hqr import entry  # noqa: E402

GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2/")
if not GAME.endswith(("/", "\\")):
    GAME += "/"
MODELS = os.path.join(os.path.dirname(TOOLS), "Resources", "Models")
WORLD_UNIT = 16384.0 / 20.0
ZOOM = 4
PALETTE = [tuple(entry(GAME + "RESS.HQR", 0)[i * 3:i * 3 + 3]) + (255,) for i in range(256)]
BOXES = entry(GAME + "RESS.HQR", 5)
LIGHT = np.array([0.45, 0.75, 0.48])  # LBA axes, towards the viewer's upper left


def load_sprite(index):
    data = entry(GAME + "SPRITES.HQR", index)
    at = struct.unpack_from("<I", data, 0)[0]
    width, height, hot_x, hot_y = data[at], data[at + 1], struct.unpack_from("b", data, at + 2)[0], struct.unpack_from("b", data, at + 3)[0]
    at += 4
    image = Image.new("RGBA", (width, height))
    pixels = image.load()
    for y in range(height):
        runs = data[at]
        at += 1
        x = 0
        for _ in range(runs):
            spec = data[at]
            at += 1
            count, kind = (spec & 63) + 1, spec >> 6
            if kind == 2:
                color = data[at]
                at += 1
                for _ in range(count):
                    if x < width:
                        pixels[x, y] = PALETTE[color]
                    x += 1
            elif kind in (1, 3):
                for _ in range(count):
                    if x < width:
                        pixels[x, y] = PALETTE[data[at]]
                    at += 1
                    x += 1
            else:
                x += count
    box = struct.unpack_from("<8h", BOXES, index * 16)
    return image, hot_x + box[0], hot_y + box[1]


def load_triangles(path):
    """World-space (LBA units) triangles with their sRGB base colors."""
    blob = open(path, "rb").read()
    json_size = struct.unpack_from("<I", blob, 12)[0]
    doc = json.loads(blob[20:20 + json_size])
    bin_start = 20 + json_size + 8

    def accessor(index):
        acc = doc["accessors"][index]
        view = doc["bufferViews"][acc["bufferView"]]
        start = bin_start + view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        kind = {5126: "<f4", 5125: "<u4", 5123: "<u2", 5121: "u1"}[acc["componentType"]]
        count = acc["count"] * {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        return np.frombuffer(blob, kind, count, start)

    triangles, colors = [], []

    def visit(index, parent):
        node = doc["nodes"][index]
        translation = np.array(node.get("translation", [0, 0, 0]), dtype=float)
        if "rotation" in node or "scale" in node or "matrix" in node:
            raise ValueError("generated doors have no node rotations or scales")
        offset = parent + translation
        if "mesh" in node:
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                positions = accessor(primitive["attributes"]["POSITION"]).reshape(-1, 3) + offset
                indices = accessor(primitive["indices"]).reshape(-1, 3)
                base = doc["materials"][primitive["material"]]["pbrMetallicRoughness"]["baseColorFactor"]
                triangles.append(positions[indices] * WORLD_UNIT)
                colors.append(np.tile(base[:3], (len(indices), 1)))
        for child in node.get("children", []):
            visit(child, offset)

    for root in doc["scenes"][0]["nodes"]:
        visit(root, np.zeros(3))
    return np.concatenate(triangles), np.concatenate(colors)


def to_srgb(linear):
    linear = np.clip(linear, 0, 1)
    return np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - 0.055)


def render(triangles, colors, offset_x, offset_y, width, height):
    """Z-buffered, in the isometric projection: x = (X - Z) * 24 / 512, y = ((X + Z) * 12 - Y * 30) / 512."""
    x = ((triangles[..., 0] - triangles[..., 2]) * 24 / 512 - offset_x) * ZOOM
    y = (((triangles[..., 0] + triangles[..., 2]) * 12 - triangles[..., 1] * 30) / 512 - offset_y) * ZOOM
    # The projection's own direction (it moves nothing on screen), towards the viewer.
    view = np.array([1.0, 0.8, 1.0])
    depth = triangles @ view
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-9)
    w, h = width * ZOOM, height * ZOOM
    nearest = np.full((h, w), -np.inf)
    image = np.zeros((h, w, 4), np.uint8)
    for i in range(len(triangles)):
        if normals[i] @ view <= 0:
            continue
        x0, x1 = max(0, int(np.floor(x[i].min()))), min(w - 1, int(np.ceil(x[i].max())))
        y0, y1 = max(0, int(np.floor(y[i].min()))), min(h - 1, int(np.ceil(y[i].max())))
        if x0 > x1 or y0 > y1:
            continue
        px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        ax, ay, bx, by, cx, cy = x[i, 0], y[i, 0], x[i, 1], y[i, 1], x[i, 2], y[i, 2]
        area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if abs(area) < 1e-9:
            continue
        wa = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / area
        wb = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / area
        wc = 1.0 - wa - wb
        inside = (wa >= 0) & (wb >= 0) & (wc >= 0)
        z = wa * depth[i, 0] + wb * depth[i, 1] + wc * depth[i, 2]
        window = nearest[y0:y1 + 1, x0:x1 + 1]
        shown = inside & (z > window)
        window[shown] = z[shown]
        shade = 0.55 + 0.75 * max(0.0, normals[i] @ LIGHT)
        color = np.clip(to_srgb(colors[i]) * shade, 0, 1)
        image[y0:y1 + 1, x0:x1 + 1][shown] = tuple(int(c * 255) for c in color) + (255,)
    return Image.fromarray(image)


def main():
    out = sys.argv[1]
    replacements = json.load(open(os.path.join(MODELS, "model_replacements.json")))["sprites"]
    tiles = []
    for key, entry_data in sorted(replacements.items(), key=lambda p: int(p[0])):
        sprite, off_x, off_y = load_sprite(int(key))
        triangles, colors = load_triangles(os.path.join(MODELS, "sprites", entry_data["file"]))
        margin = 12
        wide, tall = sprite.width + 2 * margin, sprite.height + 2 * margin
        model = render(triangles, colors, off_x - margin, off_y - margin, wide, tall)
        art = Image.new("RGBA", (wide, tall))
        art.alpha_composite(sprite, (margin, margin))
        art = art.resize((wide * ZOOM, tall * ZOOM), Image.NEAREST)
        a, b = np.array(art)[..., 3] > 0, np.array(model)[..., 3] > 0
        iou = (a & b).sum() / max(1, (a | b).sum())
        diff = np.zeros(a.shape + (4,), np.uint8)
        diff[a & b] = (255, 255, 255, 255)
        diff[a & ~b] = (255, 60, 60, 255)
        diff[~a & b] = (70, 120, 255, 255)
        tile = Image.new("RGBA", (wide * ZOOM * 3 + 16, tall * ZOOM + 20), (32, 32, 38, 255))
        tile.alpha_composite(art, (0, 20))
        tile.alpha_composite(model, (wide * ZOOM + 8, 20))
        tile.alpha_composite(Image.fromarray(diff), (2 * wide * ZOOM + 16, 20))
        ImageDraw.Draw(tile).text((2, 4), f"#{key} {entry_data['file']} overlap {iou:.3f}", fill=(255, 255, 120, 255))
        print(f"sprite {key:>3} {entry_data['file']:28s} overlap {iou:.3f}")
        tiles.append(tile)
    sheet_width = 1800
    x = y = row = 0
    places = []
    for tile in tiles:
        if x + tile.width > sheet_width:
            x, y, row = 0, y + row + 8, 0
        places.append((x, y))
        x += tile.width + 8
        row = max(row, tile.height)
    sheet = Image.new("RGBA", (sheet_width, y + row), (18, 18, 22, 255))
    for tile, place in zip(tiles, places):
        sheet.alpha_composite(tile, place)
    sheet.save(out)


main()
