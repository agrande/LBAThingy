"""
Generates 3D models of the Temple of Bu's door sprites, which lba2remake has no models for.

    blender -b -P Plugins/LBA2Importer/Tools/make_door_models.py

Writes Resources/Models/sprites/<name>.glb (read by the importer, listed in Resources/Models/model_replacements.json)
and Resources/Models/source/<name>.blend, the same model to rework by hand: export it over the GLB of the same name.

Same conventions as the remake's sprite models: remake world units (1 unit = 819.2 LBA units), origin at the object's
position, flat materials named "column,row" after their palette swatch (palette index = row * 16 + column), with that
swatch as base color. Sizes are measured on the sprites: Tools/check_door_models.py compares a model with its sprite in
the game's isometric projection.

Each model is drawn in leaf space: u along the leaf, v up, w out of its front. A leaf lies along LBA X with its front
towards +Z, or along LBA Z with its front towards +X: the two faces the isometric view shows.
"""
import math
import os
import struct
import sys

import bmesh
import bpy

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from hqr import entry  # noqa: E402

GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2/")
if not GAME.endswith(("/", "\\")):
    GAME += "/"
PLUGIN = os.path.dirname(TOOLS)
OUT_GLB = os.path.join(PLUGIN, "Resources", "Models", "sprites")
OUT_BLEND = os.path.join(PLUGIN, "Resources", "Models", "source")
WORLD_UNIT = 16384.0 / 20.0
PALETTE = entry(GAME + "RESS.HQR", 0)


def swatch_color(name):
    column, row = map(int, name.split(","))
    index = row * 16 + column

    def linear(byte):
        s = byte / 255.0
        return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4

    return tuple(linear(PALETTE[index * 3 + k]) for k in range(3)) + (1.0,)


def material(name):
    found = bpy.data.materials.get(name)
    if found:
        return found
    created = bpy.data.materials.new(name)
    created.use_nodes = True
    bsdf = created.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = swatch_color(name)
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 0.5
    created.diffuse_color = swatch_color(name)
    return created


# Outlines (counter-clockwise in u, v).

def rect(u0, u1, v0, v1):
    return [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]


def rounded_rect(u0, u1, v0, v1, radius, segments=4):
    points = []
    corners = [(u1 - radius, v0 + radius, -90), (u1 - radius, v1 - radius, 0), (u0 + radius, v1 - radius, 90), (u0 + radius, v0 + radius, 180)]
    for cu, cv, start in corners:
        for k in range(segments + 1):
            a = math.radians(start + 90.0 * k / segments)
            points.append((cu + radius * math.cos(a), cv + radius * math.sin(a)))
    return points


def ellipse(cu, cv, ru, rv, angles):
    return [(cu + ru * math.cos(a), cv + rv * math.sin(a)) for a in angles]


def clip_to_rect(polygon, u0, u1, v0, v1):
    """Sutherland-Hodgman against an axis-aligned rectangle."""
    for axis, limit, keep_below in ((0, u0, False), (0, u1, True), (1, v0, False), (1, v1, True)):
        kept = []
        for i, a in enumerate(polygon):
            b = polygon[(i + 1) % len(polygon)]
            a_in = a[axis] <= limit if keep_below else a[axis] >= limit
            b_in = b[axis] <= limit if keep_below else b[axis] >= limit
            if a_in:
                kept.append(a)
            if a_in != b_in:
                t = (limit - a[axis]) / (b[axis] - a[axis])
                kept.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
        polygon = kept
        if not polygon:
            return []
    return polygon


class Model:
    """Parts of a door, one object per (part, material), in leaf space."""

    def __init__(self, name):
        self.name = name
        self.parts = {}

    def _part(self, part, swatch, smooth):
        return self.parts.setdefault((part, swatch), {"verts": [], "faces": [], "smooth": smooth, "bevel": 0.0})

    def add(self, part, swatch, verts, faces, smooth=False, bevel=0.0):
        data = self._part(part, swatch, smooth)
        base = len(data["verts"])
        data["verts"].extend(verts)
        data["faces"].extend([tuple(i + base for i in face) for face in faces])
        data["bevel"] = max(data["bevel"], bevel)

    def prism(self, part, swatch, outline, w0, w1, bevel=0.0):
        """Extrudes a counter-clockwise outline from w0 (back) to w1 (front)."""
        n = len(outline)
        verts = [(u, v, w1) for u, v in outline] + [(u, v, w0) for u, v in outline]
        faces = [tuple(range(n)), tuple(n + i for i in reversed(range(n)))]
        for i in range(n):
            j = (i + 1) % n
            faces.append((n + i, n + j, j, i))
        self.add(part, swatch, verts, faces, bevel=bevel)

    def box(self, part, swatch, u0, u1, v0, v1, w0, w1, bevel=0.0):
        self.prism(part, swatch, rect(u0, u1, v0, v1), w0, w1, bevel)

    def ring(self, part, swatch, outer, inner, w0, w1):
        """A plate between two counter-clockwise outlines of as many points, one inside the other."""
        n = len(outer)
        ring_verts = [(u, v, w1) for u, v in outer] + [(u, v, w1) for u, v in inner] + [(u, v, w0) for u, v in outer] + [(u, v, w0) for u, v in inner]
        faces = []
        for i in range(n):
            j = (i + 1) % n
            faces.append((i, j, n + j, n + i))                         # front
            faces.append((2 * n + j, 2 * n + i, 3 * n + i, 3 * n + j))  # back
            faces.append((2 * n + i, 2 * n + j, j, i))                 # outside
            faces.append((n + i, n + j, 3 * n + j, 3 * n + i))         # inside
        self.add(part, swatch, ring_verts, faces)

    def spike(self, part, swatch, u, v0, w, radius, height, segments=12):
        verts = [(u + radius * math.cos(2 * math.pi * k / segments), v0, w + radius * math.sin(2 * math.pi * k / segments)) for k in range(segments)]
        verts.append((u, v0 + height, w))
        faces = [tuple(range(segments))]
        for k in range(segments):
            faces.append((segments, (k + 1) % segments, k))
        self.add(part, swatch, verts, faces, smooth=True)

    def hood(self, part, swatch, u, v, w, radius, depth, rows=5, segments=10):
        """Half a dome on a plate: flat side down, rounded over the top and out towards +w (or -w with a negative depth)."""
        verts = []
        for r in range(rows + 1):
            out = r / rows * math.pi / 2  # 0 on the plate, 90 degrees at the tip
            for k in range(segments + 1):
                around = math.pi * k / segments
                radial = radius * math.cos(out)
                verts.append((u + radial * math.cos(around), v + radial * math.sin(around), w + depth * math.sin(out)))
        faces = []
        stride = segments + 1
        for r in range(rows):
            for k in range(segments):
                a, b, c, d = r * stride + k, r * stride + k + 1, (r + 1) * stride + k + 1, (r + 1) * stride + k
                faces.append((a, b, c, d) if depth > 0 else (d, c, b, a))
        # The underside.
        bottom = [(rows - r) * stride for r in range(rows + 1)] + [r * stride + segments for r in range(rows)]
        faces.append(tuple(reversed(bottom)) if depth > 0 else tuple(bottom))
        self.add(part, swatch, verts, faces, smooth=True)

    def stud(self, part, swatch, u, v, w, radius, height, segments=8):
        """A small round head on a plate, towards +w (or -w with a negative height)."""
        verts = []
        for r, level in ((1.0, 0.0), (0.8, 0.7), (0.4, 1.0)):
            for k in range(segments):
                a = 2 * math.pi * k / segments
                verts.append((u + radius * r * math.cos(a), v + radius * r * math.sin(a), w + height * level))
        faces = []
        for row in range(2):
            for k in range(segments):
                j = (k + 1) % segments
                a, b, c, d = row * segments + k, row * segments + j, (row + 1) * segments + j, (row + 1) * segments + k
                faces.append((a, b, c, d) if height > 0 else (d, c, b, a))
        cap = tuple(2 * segments + k for k in range(segments))
        faces.append(cap if height > 0 else tuple(reversed(cap)))
        self.add(part, swatch, verts, faces, smooth=True)

    def bend(self, offset):
        """Moves every point along w by offset(u): a curved plate."""
        for data in self.parts.values():
            data["verts"] = [(u, v, w + offset(u)) for u, v, w in data["verts"]]

    def build(self, along, origin, front, length, flip=False):
        """Maps leaf space to LBA axes: a leaf along "X" or "Z", from `origin` over `length`, its front at `front`."""
        for (part, swatch), data in self.parts.items():
            mesh = bpy.data.meshes.new(f"{part}_{swatch.replace(',', '_')}")
            mapped = []
            for u, v, w in data["verts"]:
                along_value = origin + (length - u if flip else u)
                x, z = (along_value, front + w) if along == "X" else (front + w, along_value)
                # Blender's Z-up frame, which the glTF exporter turns into (X, Y, Z) = (x, z, -y).
                mapped.append((x / WORLD_UNIT, -z / WORLD_UNIT, v / WORLD_UNIT))
            # Along X keeps u, v, w right-handed; along Z swaps two axes, and a flip mirrors once more.
            mirrored = (along == "Z") != flip
            faces = [tuple(reversed(f)) for f in data["faces"]] if mirrored else data["faces"]
            mesh.from_pydata(mapped, [], faces)
            mesh.update()
            mesh.materials.append(material(swatch))
            mesh.use_auto_smooth = True
            mesh.auto_smooth_angle = math.radians(40.0)
            for polygon in mesh.polygons:
                polygon.use_smooth = data["smooth"] or data["bevel"] > 0.0
            obj = bpy.data.objects.new(mesh.name, mesh)
            bpy.context.scene.collection.objects.link(obj)
            if data["bevel"] > 0.0:
                bevel = obj.modifiers.new("Bevel", "BEVEL")
                bevel.width = data["bevel"] / WORLD_UNIT
                bevel.segments = 1
                bevel.limit_method = "ANGLE"
                bevel.angle_limit = math.radians(40.0)


# The designs.

def spiked(thickness):
    """Sprites 139, 140: dark studded boards with three spikes on top."""
    m = Model("spiked")
    height = 1250.0
    m.box("body", "0,14", 0, 768, 0, height, -thickness + 8, -8)
    for side_w0, side_w1 in ((-8, 0), (-thickness, -thickness + 8)):
        for i, swatch in enumerate(("3,13", "5,14", "3,3")):
            m.box("boards", swatch, i * 256 + 8, (i + 1) * 256 - 8, 6, height - 6, side_w0, side_w1)
    for u in (256, 512):
        m.box("straps", "0,7", u - 12, u + 12, 0, height, -thickness - 3, 3)
    for i in range(3):
        center = i * 256 + 128
        for v in (204.0, 900.0):
            m.hood("studs", "5,3", center, v, 0, 78, 44)
            m.hood("studs", "5,3", center, v, -thickness, 78, -44)
            m.box("stud_lips", "9,3", center - 78, center + 78, v - 10, v, 0, 44)
            m.box("stud_lips", "9,3", center - 78, center + 78, v - 10, v, -thickness - 44, -thickness)
        m.spike("spikes", "7,11", center, height, -40, 46, 370)
        m.box("sockets", "3,3", center - 52, center + 52, height, height + 24, -thickness + 8, -8)
    return m


def arch_top(u_from_center):
    """The arch two leaves make: upright up to 1150, then a half ellipse up to 1860."""
    t = min(1.0, abs(u_from_center) / 768.0)
    return 1150.0 + 710.0 * math.sqrt(1.0 - t * t)


def arch_outline(u0, u1, center, step=48.0):
    """Outline of the part of the arch between u0 and u1 (center = where the two leaves meet)."""
    points = [(u0, 0.0), (u1, 0.0)]
    count = max(1, int(abs(u1 - u0) / step))
    for k in range(count + 1):
        u = u1 + (u0 - u1) * k / count
        points.append((u, arch_top(u - center)))
    return points


def arched(meets_at_end):
    """Sprites 325-328: half of a wooden arch with a porthole; the two leaves meet at the arch's top."""
    m = Model("arched")
    thickness = 106.0
    center = 768.0 if meets_at_end else 0.0
    m.prism("body", "0,14", arch_outline(0, 768, center), -thickness + 10, -10)
    boards = ("5,1", "1,6", "6,1", "2,1", "4,1")
    width = 768.0 / len(boards)
    for i, swatch in enumerate(boards):
        outline = arch_outline(i * width + 16, (i + 1) * width - 16, center)
        m.prism("boards", swatch, outline, -10, 0)
        m.prism("boards", swatch, outline, -thickness, -thickness + 10)
    angles = [2 * math.pi * k / 32 for k in range(32)]
    for w, sign in ((0.0, 1.0), (-thickness, -1.0)):
        outer = ellipse(384, 1280, 235, 235, angles)
        inner = ellipse(384, 1280, 190, 190, angles)
        w0, w1 = sorted((w, w + 24 * sign))
        m.ring("porthole_ring", "5,6", outer, inner, w0, w1)
        g0, g1 = sorted((w, w + 8 * sign))
        m.prism("porthole_glass", "3,12", inner, g0, g1)
    return m


def cell(with_keyhole):
    """Sprites 332, 169: iron cell door: four slits you can see through over a panel, with or without a keyhole."""
    m = Model("cell")
    thickness = 70.0
    width, height = 1024.0, 1985.0
    outer = rounded_rect(0, width, 0, height, 60)
    inner = rounded_rect(70, width - 70, 70, height - 70, 40)
    m.ring("rim", "3,11", outer, inner, -thickness, 0)
    # The plate, holed by the window's slits (measured on the sprite: its pixels between the bars are transparent).
    back, front = -thickness + 12.0, -12.0
    slits, half_slit = (311.0, 460.0, 610.0, 759.0), 35.0
    window = (218.0, 922.0, 1160.0, 1805.0)
    m.box("plate", "3,13", 60, width - 60, 60, window[2], back, front)
    m.box("plate", "3,13", 60, width - 60, window[3], height - 60, back, front)
    m.box("plate", "3,13", 60, window[0], window[2], window[3], back, front)
    m.box("plate", "3,13", window[1], width - 60, window[2], window[3], back, front)
    edges = [window[0]] + [e for slit in slits for e in (slit - half_slit, slit + half_slit)] + [window[1]]
    for k in range(0, len(edges), 2):
        m.box("window_bars", "2,3", edges[k], edges[k + 1], window[2], window[3], -43.0, -27.0, bevel=5)
    for side, sign in ((front, 1.0), (back, -1.0)):
        def layer(a, b):
            return sorted((side + a * sign, side + b * sign))
        w0, w1 = layer(0, 6)
        for u0, u1, v0, v1 in ((window[0] - 30, window[1] + 30, window[2] - 30, window[2]), (window[0] - 30, window[1] + 30, window[3], window[3] + 30),
                               (window[0] - 30, window[0], window[2], window[3]), (window[1], window[1] + 30, window[2], window[3])):
            m.box("window_frame", "5,14", u0, u1, v0, v1, w0, w1)
        p0, p1 = layer(0, 5)
        m.prism("panel_frame", "3,13", rounded_rect(130, width - 130, 130, 1090, 50), p0, p1, bevel=3)
        p0, p1 = layer(5, 9)
        m.prism("panel", "3,11", rounded_rect(160, width - 160, 160, 1060, 40), p0, p1, bevel=2)
    if with_keyhole:
        # Keyhole on the front only.
        w0, w1 = front + 9.0, front + 10.5
        angles = [2 * math.pi * k / 12 for k in range(12)]
        m.prism("keyhole", "0,14", ellipse(528, 830, 30, 30, angles), w0, w1)
        m.prism("keyhole", "0,14", [(510, 730), (546, 730), (546, 830), (510, 830)], w0, w1)
    return m


def lattice(handle_at):
    """Sprites 114, 115: teal door with a lattice at the top, a handle and a band of metal mesh at the bottom."""
    m = Model("lattice")
    thickness = 70.0
    width, height = 1024.0, 1504.0
    u0, u1, v0, v1 = 40.0, width - 40.0, 1010.0, height - 45.0
    # Teal around the lattice's frame, pale behind its holes.
    m.box("plate", "11,11", 0, width, 400, v0, -thickness, 0, bevel=4)
    m.box("plate", "11,11", 0, width, v1, height, -thickness, 0, bevel=4)
    m.box("plate", "11,11", 0, u0, v0, v1, -thickness, 0)
    m.box("plate", "11,11", u1, width, v0, v1, -thickness, 0)
    m.box("lattice_back", "12,13", u0, u1, v0, v1, -thickness + 22, -22)
    spacing, half = 240.0, 26.0
    for side0, side1 in ((-22.0, 0.0), (-thickness, -thickness + 22.0)):
        for k in range(-6, 12):
            for sign in (1.0, -1.0):
                base = k * spacing
                far = 1000.0
                bar = [(base - far, v0 - sign * far - half), (base + far, v0 + sign * far - half), (base + far, v0 + sign * far + half), (base - far, v0 - sign * far + half)]
                if sign < 0:
                    bar = list(reversed(bar))
                clipped = clip_to_rect(bar, u0, u1, v0, v1)
                if len(clipped) >= 3:
                    m.prism("lattice", "10,11", clipped, side0, side1)
    # The band of mesh.
    m.box("mesh_plate", "5,13", 0, width, 0, 400, -thickness - 2, 2)
    m.box("mesh_trim", "4,3", 0, width, 380, 400, -thickness - 4, 4)
    spacing, half = 64.0, 7.0
    for k in range(-8, 24):
        for sign in (1.0, -1.0):
            base = k * spacing
            far = 500.0
            bar = [(base - far, -sign * far - half), (base + far, sign * far - half), (base + far, sign * far + half), (base - far, -sign * far + half)]
            if sign < 0:
                bar = list(reversed(bar))
            clipped = clip_to_rect(bar, 0, width, 0, 380)
            if len(clipped) >= 3:
                m.prism("mesh", "6,3", clipped, 2, 7)
    for k in range(5):
        u = 110 + k * (width - 220) / 4
        m.stud("rivets", "8,3", u, 345, 7, 14, 10)
    # Handle and lock, on both sides.
    for side, sign in ((0.0, 1.0), (-thickness, -1.0)):
        count = 8
        verts, faces = [], []
        for k in range(count + 1):
            t = k / count
            bulge = math.sin(math.pi * t)
            u = handle_at + (handle_at > width / 2 and 30.0 or -30.0) * bulge
            v = 900.0 - 300.0 * t
            w = side + sign * (8.0 + 46.0 * bulge)
            half_u, half_w = 26.0, 14.0
            verts += [(u - half_u, v, w - half_w), (u + half_u, v, w - half_w), (u + half_u, v, w + half_w), (u - half_u, v, w + half_w)]
        for k in range(count):
            for c in range(4):
                d = (c + 1) % 4
                a = k * 4 + c
                faces.append((a, k * 4 + d, (k + 1) * 4 + d, (k + 1) * 4 + c))
        faces += [(3, 2, 1, 0), tuple(count * 4 + c for c in range(4))]
        m.add("handle", "3,3", verts, faces, smooth=True)
        m.stud("lock", "4,3", handle_at, 520, side, 34, 8 * sign)
        m.stud("lock", "0,14", handle_at, 520, side + 7 * sign, 16, 3 * sign)
    return m


def hatch():
    """Sprite 209: a curved plate with a great oval hole."""
    m = Model("hatch")
    thickness = 128.0
    width, height = 1024.0, 1740.0
    cu, cv, ru, rv = 491.0, 916.0, 455.0, 565.0
    # Same angles for the hole and the plate's outline, corners included.
    corners = [math.atan2(v - cv, u - cu) % (2 * math.pi) for u, v in ((width, 0), (width, height), (0, height), (0, 0))]
    angles = sorted(set(corners + [2 * math.pi * k / 64 for k in range(64)]))

    def on_outline(a):
        dx, dy = math.cos(a), math.sin(a)
        reach = []
        if dx > 1e-6:
            reach.append((width - cu) / dx)
        if dx < -1e-6:
            reach.append(-cu / dx)
        if dy > 1e-6:
            reach.append((height - cv) / dy)
        if dy < -1e-6:
            reach.append(-cv / dy)
        r = min(reach)
        return (cu + dx * r, cv + dy * r)

    outline = [on_outline(a) for a in angles]
    hole = ellipse(cu, cv, ru, rv, angles)
    trim = ellipse(cu, cv, ru + 44, rv + 44, angles)
    m.ring("plate", "5,11", outline, trim, -thickness, 0)
    m.ring("hole_trim", "11,11", trim, hole, -thickness - 8, 12)
    m.box("end_cap", "11,14", width - 26, width + 2, -2, height + 2, -thickness - 4, 8)
    m.bend(lambda u: 128.0 - 64.0 * ((u - 512.0) / 512.0) ** 2)
    return m


# name, sprite, design, leaf along, origin along the leaf, front, length, mirrored along the leaf.
DOORS = [
    ("door_spiked_x", lambda: spiked(160.0), "X", 0.0, 0.0, 768.0, False),
    ("door_spiked_z", lambda: spiked(128.0), "Z", 0.0, 21.0, 768.0, False),
    ("door_arch_left_x", lambda: arched(True), "X", 0.0, 0.0, 768.0, False),
    ("door_arch_right_x", lambda: arched(False), "X", 0.0, 0.0, 768.0, False),
    ("door_arch_right_z", lambda: arched(False), "Z", 0.0, 21.0, 768.0, False),
    ("door_arch_left_z", lambda: arched(True), "Z", 0.0, 21.0, 768.0, False),
    ("door_cell_keyhole", lambda: cell(True), "Z", -16.0, 10.0, 1024.0, False),
    ("door_cell", lambda: cell(False), "Z", -16.0, 10.0, 1024.0, False),
    ("door_lattice_a", lambda: lattice(216.0), "Z", -16.0, 10.0, 1024.0, False),
    ("door_lattice_b", lambda: lattice(808.0), "Z", -16.0, 10.0, 1024.0, False),
    ("door_hatch", hatch, "X", -512.0, 0.0, 1024.0, False),
]


def main():
    os.makedirs(OUT_GLB, exist_ok=True)
    os.makedirs(OUT_BLEND, exist_ok=True)
    wanted = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else None
    for name, design, along, origin, front, length, flip in DOORS:
        if wanted and name not in wanted:
            continue
        bpy.ops.wm.read_factory_settings(use_empty=True)
        design().build(along, origin, front, length, flip)
        bpy.ops.export_scene.gltf(filepath=os.path.join(OUT_GLB, name + ".glb"), export_format="GLB", export_apply=True,
                                  export_yup=True, export_normals=True, export_materials="EXPORT", export_animations=False)
        # No .blend1 backups next to the sources.
        bpy.context.preferences.filepaths.save_version = 0
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT_BLEND, name + ".blend"), compress=True)
        print("DOOR", name)


main()
