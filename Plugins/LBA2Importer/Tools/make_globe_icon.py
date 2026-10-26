"""Draws the editor toolbar's Globe button icon (a little sea planet with an island on it, in the sticker style of the
other icons) to Resources/Icons/Globe.png. Needs Pillow:
  python make_globe_icon.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter

TOOLS = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(TOOLS)

SIZE = 40
S = 8  # supersampling
W = SIZE * S


def box(x, y, r):
    return [(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S]


def make_globe_icon(path):
    image = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    planet = Image.new("L", (W, W), 0)
    ImageDraw.Draw(planet).ellipse(box(20, 20, 15), fill=255)
    # Sticker: white outline, dark ink line, then the sea.
    image.paste((255, 255, 255, 255), (0, 0), planet.filter(ImageFilter.MaxFilter(4 * S + 1)))
    image.paste((30, 40, 48, 255), (0, 0), planet.filter(ImageFilter.MaxFilter(S + 1)))
    image.paste((40, 110, 170, 255), (0, 0), planet)
    # Lit side: a lighter sea towards the top left, clipped to the planet.
    light = Image.new("L", (W, W), 0)
    ImageDraw.Draw(light).ellipse(box(16, 15, 11), fill=255)
    light = Image.composite(light, Image.new("L", (W, W), 0), planet)
    image.paste((80, 170, 220, 255), (0, 0), light.filter(ImageFilter.GaussianBlur(2 * S)))
    # An island on the curve: sand, then grass.
    island = Image.new("L", (W, W), 0)
    ImageDraw.Draw(island).ellipse([12 * S, 17 * S, 28 * S, 26 * S], fill=255)
    island = Image.composite(island, Image.new("L", (W, W), 0), planet)
    image.paste((232, 205, 140, 255), (0, 0), island)
    grass = Image.new("L", (W, W), 0)
    ImageDraw.Draw(grass).ellipse([15 * S, 18 * S, 25 * S, 24 * S], fill=255)
    image.paste((90, 170, 80, 255), (0, 0), grass)
    # Glint on the sea.
    draw = ImageDraw.Draw(image)
    draw.arc(box(20, 20, 12), 200, 245, fill=(235, 250, 255, 255), width=2 * S)
    image.resize((SIZE, SIZE), Image.LANCZOS).save(path)


if __name__ == "__main__":
    icons = os.path.join(PLUGIN, "Resources", "Icons")
    os.makedirs(icons, exist_ok=True)
    out = os.path.join(icons, "Globe.png")
    make_globe_icon(out)
    print("written", out)
