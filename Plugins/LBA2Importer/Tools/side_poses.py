"""Prints shots.ps1 poses that show every side and corner of a grid's rooms, from the room model dump the import
writes (Saved/LookDev/enclosure/cells/gridNNN.txt): per room, one shot of each side (-X, +X, -Z, +Z) from the
opposite side, then one of each corner from the opposite corner, level, at eye height over the room's floor.
Usage: side_poses.py <grid> [room ...]"""
import math, os, sys

CELL_XZ, CELL_Y = 64.0, 32.0     # cm at the default world scale
EYE = 150.0
DUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Saved", "LookDev", "enclosure", "cells")

grid = int(sys.argv[1])
only = {int(a) for a in sys.argv[2:]}
rooms = {}
for line in open(os.path.join(DUMP, f"grid{grid:03d}.txt")):
    p = line.split()
    if p[0] == "C":
        x, z, room, floor = int(p[1]), int(p[2]), int(p[3]), int(p[4])
        rooms.setdefault(room, []).append((x, z, floor))
poses = []
for room, cols in sorted(rooms.items()):
    if only and room not in only:
        continue
    xs = [c[0] for c in cols]; zs = [c[1] for c in cols]
    floors = sorted(c[2] for c in cols)
    eye = (floors[len(floors) // 2] + 1) * CELL_Y + EYE
    x0, x1, z0, z1 = min(xs) + 1.0, max(xs), min(zs) + 1.0, max(zs)     # one cell in from the box's edges (cells)
    cx, cz = (min(xs) + max(xs) + 1) / 2, (min(zs) + max(zs) + 1) / 2
    # Sides: (camera, yaw) looking at -X, +X, -Z, +Z.
    shots = [((x1, cz), 180), ((x0, cz), 0), ((cx, z1), -90), ((cx, z0), 90)]
    # Corners: from the opposite corner.
    for tx, tz in ((min(xs), min(zs)), (max(xs) + 1, min(zs)), (min(xs), max(zs) + 1), (max(xs) + 1, max(zs) + 1)):
        fx = x1 if tx == min(xs) else x0
        fz = z1 if tz == min(zs) else z0
        shots.append(((fx, fz), math.degrees(math.atan2(tz - fz, tx - fx))))
    for (x, z), yaw in shots:
        poses.append(f"{x * CELL_XZ:.0f},{z * CELL_XZ:.0f},{eye:.0f},0,{yaw:.0f}")
print(";".join(poses))
