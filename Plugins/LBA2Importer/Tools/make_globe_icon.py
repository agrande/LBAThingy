"""Draws the editor toolbar's Globe button icon (a little sea planet with an island on it, in the sticker style of the
other icons) to Resources/Icons/Globe.png, and the Packaged button's (the same with a shipping box in front) to
Resources/Icons/GlobePackaged.png. Needs Pillow:
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


def draw_globe():
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
    return image


def draw_box(image):
    """A cardboard box in the bottom right corner, in front of the planet: the packaged game."""
    front = [(21 * S, 25 * S), (31 * S, 25 * S), (31 * S, 36 * S), (21 * S, 36 * S)]
    side = [(31 * S, 25 * S), (36 * S, 21 * S), (36 * S, 32 * S), (31 * S, 36 * S)]
    top = [(21 * S, 25 * S), (26 * S, 21 * S), (36 * S, 21 * S), (31 * S, 25 * S)]
    shape = Image.new("L", (W, W), 0)
    ImageDraw.Draw(shape).polygon(front + side[1:3] + top[1:2], fill=255)
    image.paste((255, 255, 255, 255), (0, 0), shape.filter(ImageFilter.MaxFilter(4 * S + 1)))
    image.paste((30, 40, 48, 255), (0, 0), shape.filter(ImageFilter.MaxFilter(S + 1)))
    draw = ImageDraw.Draw(image)
    draw.polygon(front, fill=(196, 140, 78, 255))
    draw.polygon(side, fill=(158, 106, 54, 255))
    draw.polygon(top, fill=(222, 176, 112, 255))
    # Tape over the top and down the front.
    draw.polygon([(24 * S, 25 * S), (29 * S, 21 * S), (31 * S, 21 * S), (26 * S, 25 * S)], fill=(240, 222, 170, 255))
    draw.rectangle([24 * S, 25 * S, 26 * S, 29 * S], fill=(240, 222, 170, 255))


def make_globe_icon(path, packaged=False):
    image = draw_globe()
    if packaged:
        draw_box(image)
    image.resize((SIZE, SIZE), Image.LANCZOS).save(path)


if __name__ == "__main__":
    icons = os.path.join(PLUGIN, "Resources", "Icons")
    os.makedirs(icons, exist_ok=True)
    for name, packaged in (("Globe.png", False), ("GlobePackaged.png", True)):
        out = os.path.join(icons, name)
        make_globe_icon(out, packaged)
        print("written", out)
