"""Draws the travel menu's Tests icon (a flask, in the sticker style of lba2remake's location icons) to
Saved/LookDev/locations/T_Location_Tests.png, for import_location_icons.py, and the editor toolbar's Levels button icon
(the Twinsun location icon) to Resources/Icons/LevelSelector.png. Needs Pillow:
  python make_tests_icon.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter

TOOLS = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(TOOLS)
PROJECT = os.path.dirname(os.path.dirname(PLUGIN))
LOCATIONS = os.path.join(PROJECT, "Saved", "LookDev", "locations")

SIZE = 96
S = 4  # supersampling
W = SIZE * S


def p(points):
    return [(x * S, y * S) for x, y in points]


def flask():
    # Neck, shoulders, wide flat bottom with rounded corners (in 96 px units).
    return [(38, 10), (58, 10), (58, 14), (55, 14), (55, 36), (78, 72), (80, 80), (76, 86), (20, 86), (16, 80), (18, 72),
            (41, 36), (41, 14), (38, 14)]


def make_tests_icon(path):
    image = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    shape = p(flask())
    # Sticker: white outline around everything, then a dark ink line, then glass and liquid.
    outline = Image.new("L", (W, W), 0)
    ImageDraw.Draw(outline).polygon(shape, fill=255)
    outline = outline.filter(ImageFilter.MaxFilter(4 * S + 1))
    image.paste((255, 255, 255, 255), (0, 0), outline)
    ink = Image.new("L", (W, W), 0)
    ImageDraw.Draw(ink).polygon(shape, fill=255)
    ink = ink.filter(ImageFilter.MaxFilter(S + 1))
    image.paste((30, 40, 48, 255), (0, 0), ink)
    draw.polygon(shape, fill=(200, 232, 240, 255))
    # Liquid below the shoulders, its surface tilted a little.
    liquid = Image.new("L", (W, W), 0)
    ImageDraw.Draw(liquid).polygon(shape, fill=255)
    cut = Image.new("L", (W, W), 0)
    ImageDraw.Draw(cut).polygon(p([(0, 58), (96, 54), (96, 96), (0, 96)]), fill=255)
    liquid = Image.composite(liquid, Image.new("L", (W, W), 0), cut)
    image.paste((97, 206, 206, 255), (0, 0), liquid)
    draw = ImageDraw.Draw(image)
    for x, y, r in [(36, 72, 4), (50, 66, 3), (58, 77, 5), (44, 80, 2)]:
        draw.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=(225, 250, 250, 255))
    # Glint on the glass.
    draw.line(p([(46, 18), (46, 34)]), fill=(255, 255, 255, 255), width=3 * S)
    image.resize((SIZE, SIZE), Image.LANCZOS).save(path)


def make_toolbar_icon(source, path):
    Image.open(source).convert("RGBA").resize((40, 40), Image.LANCZOS).save(path)


if __name__ == "__main__":
    os.makedirs(LOCATIONS, exist_ok=True)
    make_tests_icon(os.path.join(LOCATIONS, "T_Location_Tests.png"))
    icons = os.path.join(PLUGIN, "Resources", "Icons")
    os.makedirs(icons, exist_ok=True)
    make_toolbar_icon(os.path.join(LOCATIONS, "T_Location_Twinsun.png"), os.path.join(icons, "LevelSelector.png"))
    print("written", os.path.join(LOCATIONS, "T_Location_Tests.png"), os.path.join(icons, "LevelSelector.png"))
