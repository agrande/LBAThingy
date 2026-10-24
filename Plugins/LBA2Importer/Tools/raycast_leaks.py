"""Leaks of an interior's enclosure, by ray casting the exported meshes (export_enclosure_obj.py) from inside its rooms.
Rays from sample points of the rooms (the plan's air dumped by every import: "C x z room floor top" lines for rooms,
"K ..." for connectors) that hit nothing escape; each is reported at the first cell it crosses that is no air.
Usage: raycast_leaks.py <grid> [points=60] [dirs=300]"""
import collections, math, os, sys
import numpy as np

LOOKDEV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Saved", "LookDev")
gi = int(sys.argv[1]); npts = int(sys.argv[2]) if len(sys.argv) > 2 else 60; ndirs = int(sys.argv[3]) if len(sys.argv) > 3 else 300
obj_dir = os.path.join(LOOKDEV, "enclosure", "obj")

V = []; F = []
for name in (f"SM_Grid_{gi:03d}.obj", f"SM_Grid_{gi:03d}_Enclosure.obj", f"SM_Grid_{gi:03d}_Ceiling.obj"):
    if not os.path.exists(os.path.join(obj_dir, name)):
        continue
    base = len(V)
    for line in open(os.path.join(obj_dir, name)):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            V.append(list(map(float, p[1:4])))
        elif p[0] == "f":
            f = [int(t.split("/")[0]) - 1 + base for t in p[1:]]
            for k in range(1, len(f) - 1):
                F.append([f[0], f[k], f[k + 1]])
V = np.array(V)[:, [0, 2, 1]]  # OBJ (x, up, y) -> Unreal (x, y, up)
T = V[np.array(F)]
A = T[:, 0]; E1 = T[:, 1] - A; E2 = T[:, 2] - A

air = {}; starts = []
for line in open(os.path.join(LOOKDEV, "enclosure", "cells", f"grid{gi:03d}.txt")):
    k, *v = line.split()
    if k in ("C", "K") and len(v) == 5:
        x, z, owner, floor, top = map(int, v)
        for y in range(floor + 1, top):
            air[(x, y, z)] = owner
        if k == "C":
            starts += [(x, y, z) for y in range(floor + 2, min(floor + 6, top))]
rng = np.random.default_rng(1)
cells = starts
points = [cells[i] for i in rng.choice(len(cells), min(npts, len(cells)), replace=False)]
# Fibonacci sphere directions.
k = np.arange(ndirs) + 0.5
phi = np.arccos(1 - 2 * k / ndirs); theta = math.pi * (1 + 5 ** 0.5) * k
dirs = np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], 1)

def hits(o, d):
    P = np.cross(d, E2); det = (E1 * P).sum(1); ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1 / np.where(ok, det, 1), 0); Tv = o - A
    u = (Tv * P).sum(1) * inv; Q = np.cross(Tv, E1); v = (Q * d).sum(1) * inv; t = (E2 * Q).sum(1) * inv
    return (ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0.01)).any()

escapes = collections.Counter(); down = 0; total = 0
for c in points:
    o = np.array([(c[0] + 0.5) * 64, (c[2] + 0.5) * 64, (c[1] + 0.5) * 32])
    for d in dirs:
        total += 1
        if hits(o, d):
            continue
        if d[2] < -0.3:
            down += 1  # down through the floors: the grid has no undersides (only seen through gaps under stairs)
            continue
        for s in np.arange(4, 6000, 4):
            pt = o + s * d
            cell = (int(pt[0] // 64), int(pt[2] // 32), int(pt[1] // 64))
            if cell not in air:
                escapes[cell] += 1
                break
n = sum(escapes.values())
print(f"grid {gi}: {n}/{total} rays escape sideways or up ({100 * n / total:.2f}%; {down} more down), through {len(escapes)} cells; most: {escapes.most_common(15)}")
