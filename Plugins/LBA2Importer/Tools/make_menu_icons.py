"""Draws the travel menu's icons (T_Menu_*: tabs, islands, areas, interiors, places) to Saved/LookDev/menu_icons, for
import_location_icons.py. Flat shapes with a dark ink outline and a light top edge, in the warm/cool colors of the
original's menus, readable at 20-32 px (the remake's sticker icons are no longer used). Needs Pillow:
  python make_menu_icons.py
"""
import math
import os
from PIL import Image, ImageChops, ImageDraw, ImageFilter

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(os.path.dirname(TOOLS)))
OUT = os.path.join(PROJECT, "Saved", "LookDev", "menu_icons")

SIZE = 64
S = 4  # supersampling
W = SIZE * S
INK = (14, 18, 30, 255)


def P(points):
    return [(x * S, y * S) for x, y in points]


def box(x, y, r):
    return [(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S]


class Filled:
    """ImageDraw whose shapes are filled unless told otherwise (shape masks)."""

    def __init__(self, draw):
        self.draw = draw

    def __getattr__(self, name):
        method = getattr(self.draw, name)
        if name in ("ellipse", "polygon", "rectangle", "pieslice"):
            return lambda *args, **kwargs: method(*args, **{"fill": 255, **kwargs})
        return method


class Icon:
    def __init__(self):
        self.image = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        self.mask = Image.new("L", (W, W), 0)

    def shape(self, draw_fn, fill):
        """Draws a filled shape and adds it to the silhouette that gets the ink outline."""
        layer = Image.new("L", (W, W), 0)
        draw_fn(Filled(ImageDraw.Draw(layer)))
        self.mask = ImageChops.lighter(self.mask, layer)
        self.image.paste(fill, (0, 0), layer)
        return layer

    def detail(self, draw_fn):
        draw_fn(ImageDraw.Draw(self.image))

    def save(self, name):
        ink = self.mask.filter(ImageFilter.MaxFilter(2 * S + 1))
        out = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        out.paste(INK, (0, 0), ink)
        out.alpha_composite(self.image)
        out.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))


def planet(name, body, bands, light, extra=None):
    icon = Icon()
    disc = icon.shape(lambda d: d.ellipse(box(32, 34, 22)), body)
    # Bands clipped to the disc.
    layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for y, h, color in bands:
        d.rectangle([0, y * S, W, (y + h) * S], fill=color)
    icon.image.paste(layer, (0, 0), Image.composite(layer.split()[3], Image.new("L", (W, W), 0), disc))
    # Light rim (top left) and shade (bottom right).
    shade = Image.new("L", (W, W), 0)
    ImageDraw.Draw(shade).ellipse(box(38, 40, 22), fill=255)
    shade = Image.composite(Image.new("L", (W, W), 0), disc, shade)
    icon.image.paste(light, (0, 0), shade)
    if extra:
        extra(icon)
    icon.save(name)


def suns(icon):
    icon.shape(lambda d: d.ellipse(box(52, 12, 8)), (255, 214, 80, 255))
    icon.shape(lambda d: d.ellipse(box(12, 10, 5)), (255, 150, 60, 255))


def craters(icon):
    icon.detail(lambda d: [d.ellipse(box(x, y, r), fill=(40, 120, 80, 255)) for x, y, r in [(24, 28, 5), (38, 44, 4), (40, 26, 3)]])


def ring(icon):
    icon.detail(lambda d: d.arc(box(32, 34, 29), 200, 340, fill=(250, 220, 250, 255), width=3 * S))


def undergas(icon):
    # Lightning under the clouds.
    icon.shape(lambda d: d.polygon(P([(34, 36), (26, 48), (32, 48), (28, 60), (40, 44), (34, 44), (38, 36)])), (255, 230, 90, 255))


def island():
    icon = Icon()
    icon.shape(lambda d: d.ellipse(box(32, 50, 26)), (40, 120, 200, 255))
    icon.shape(lambda d: d.pieslice([10 * S, 26 * S, 54 * S, 62 * S], 180, 360), (96, 176, 72, 255))
    icon.detail(lambda d: d.pieslice([10 * S, 26 * S, 54 * S, 62 * S], 180, 230, fill=(150, 210, 110, 255)))
    # Palm.
    icon.shape(lambda d: d.line(P([(36, 40), (40, 26), (44, 16)]), fill=255, width=3 * S), (120, 80, 40, 255))
    for a in (-160, -110, -60, -10):
        r = math.radians(a)
        tip = (44 + 13 * math.cos(r), 16 + 9 * math.sin(r) + 6)
        icon.shape(lambda d, tip=tip: d.line(P([(44, 16), ((44 + tip[0]) / 2, 13), tip]), fill=255, width=3 * S), (60, 150, 60, 255))
    icon.save("T_Menu_Island")


def section():
    # A signpost, like the original's road signs.
    icon = Icon()
    icon.shape(lambda d: d.rectangle(P([(29, 18), (35, 60)])), (150, 100, 50, 255))
    icon.shape(lambda d: d.polygon(P([(10, 12), (46, 12), (56, 20), (46, 28), (10, 28)])), (232, 190, 110, 255))
    icon.detail(lambda d: d.line(P([(16, 20), (42, 20)]), fill=(120, 70, 30, 255), width=2 * S))
    icon.shape(lambda d: d.polygon(P([(54, 32), (18, 32), (8, 40), (18, 48), (54, 48)])), (200, 150, 80, 255))
    icon.detail(lambda d: d.line(P([(22, 40), (48, 40)]), fill=(110, 64, 28, 255), width=2 * S))
    icon.save("T_Menu_Section")


def building():
    icon = Icon()
    icon.shape(lambda d: d.rectangle(P([(14, 30), (50, 58)])), (236, 226, 200, 255))
    icon.shape(lambda d: d.polygon(P([(8, 32), (32, 10), (56, 32)])), (200, 70, 50, 255))
    icon.detail(lambda d: d.polygon(P([(8, 32), (32, 10), (34, 13), (12, 32)]), fill=(240, 120, 90, 255)))
    icon.shape(lambda d: d.rectangle(P([(27, 42), (37, 58)])), (110, 70, 40, 255))
    icon.shape(lambda d: d.rectangle(P([(18, 36), (24, 42)])), (90, 170, 220, 255))
    icon.shape(lambda d: d.rectangle(P([(40, 36), (46, 42)])), (90, 170, 220, 255))
    icon.save("T_Menu_Building")


def tests():
    icon = Icon()
    flask = P([(26, 6), (38, 6), (38, 10), (36, 10), (36, 26), (54, 52), (54, 58), (10, 58), (10, 52), (28, 26), (28, 10), (26, 10)])
    icon.shape(lambda d: d.polygon(flask), (210, 230, 240, 255))
    liquid = Image.new("L", (W, W), 0)
    ImageDraw.Draw(liquid).polygon(flask, fill=255)
    cut = Image.new("L", (W, W), 0)
    ImageDraw.Draw(cut).rectangle(P([(0, 38), (64, 64)]), fill=255)
    icon.image.paste((80, 210, 150, 255), (0, 0), Image.composite(liquid, Image.new("L", (W, W), 0), cut))
    icon.detail(lambda d: [d.ellipse(box(x, y, r), fill=(220, 255, 230, 255)) for x, y, r in [(24, 50, 3), (36, 46, 2), (40, 53, 3)]])
    icon.save("T_Menu_Tests")


def points():
    # A gold star, the showcase places.
    icon = Icon()
    star = []
    for i in range(10):
        a = math.radians(-90 + i * 36)
        r = 28 if i % 2 == 0 else 12
        star.append((32 + r * math.cos(a), 34 + r * math.sin(a)))
    icon.shape(lambda d: d.polygon(P(star)), (255, 200, 60, 255))
    icon.detail(lambda d: d.polygon(P([star[9], star[0], star[1], (32, 34)]), fill=(255, 236, 150, 255)))
    icon.save("T_Menu_Points")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    planet("T_Menu_Twinsun", (60, 140, 220, 255), [(36, 6, (90, 180, 90, 255)), (46, 4, (90, 180, 90, 255))], (30, 80, 150, 255), suns)
    planet("T_Menu_Moon", (110, 210, 140, 255), [], (50, 140, 90, 255), craters)
    planet("T_Menu_Zeelish", (200, 120, 200, 255), [(22, 5, (240, 170, 220, 255)), (34, 4, (150, 80, 170, 255)), (44, 5, (240, 170, 220, 255))],
           (120, 60, 140, 255), ring)
    planet("T_Menu_Undergas", (90, 70, 150, 255), [(12, 16, (220, 200, 240, 255)), (28, 3, (180, 160, 220, 255))], (50, 40, 100, 255), undergas)
    island()
    section()
    building()
    tests()
    points()
    print("written", OUT)
