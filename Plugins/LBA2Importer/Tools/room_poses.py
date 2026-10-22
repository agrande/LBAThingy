"""Prints shots.ps1 poses inside the largest room of a grid: from near each corner of its floor, eye height, looking
at the room's center (slightly down). -spread: points spread over the floor (farthest-point sampling, away from
walls), each looking at the floor's center, at its own floor's eye height.
Usage: room_poses.py <grid> [count] [-spread]"""
import math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_grids as S

CELL_XZ, CELL_Y = 64.0, 32.0     # cm at the default world scale
EYE = 150.0

g = S.Grid(int(sys.argv[1]), {})
count = int(sys.argv[2]) if len(sys.argv) > 2 else 4
space = max(g.spaces(), key=len)
pts = np.array([(x, z) for x, _, z in space], float)
floor = int(np.median([y for _, y, _ in space]))
center = pts.mean(axis=0)
poses = []
if "-spread" in sys.argv:
    cells = {(x, z): y for x, y, z in space}
    # Away from walls: cells with floor all around (2 cells).
    inner = [c for c in cells if all((c[0] + dx, c[1] + dz) in cells for dx in (-2, 0, 2) for dz in (-2, 0, 2))]
    if len(inner) < count * 3:
        inner = [c for c in cells if all((c[0] + dx, c[1] + dz) in cells for dx in (-1, 0, 1) for dz in (-1, 0, 1))] or list(cells)
    picks = [min(inner, key=lambda c: (c[0] - center[0]) ** 2 + (c[1] - center[1]) ** 2)]
    while len(picks) < count:
        picks.append(max(inner, key=lambda c: min((c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2 for p in picks)))
    def free(c, a):
        """Cells of clear view from c along angle a at eye height (2-4 cells above its floor)."""
        y0 = cells[c] + 2; d = 0.0
        while d < 40:
            d += 0.5
            x, z = int(c[0] + 0.5 + math.cos(a) * d), int(c[1] + 0.5 + math.sin(a) * d)
            if not (0 <= x < 64 and 0 <= z < 64) or g.solid[x, y0:y0 + 3, z].any():
                break
        return d
    for c in picks:
        # Looking along the longest clear line of sight.
        best = max((k * math.pi / 12 for k in range(24)), key=lambda a: free(c, a))
        look = np.array([math.cos(best), math.sin(best)])
        yaw = math.degrees(math.atan2(look[1], look[0]))
        poses.append(f"{(c[0] + 0.5) * CELL_XZ:.0f},{(c[1] + 0.5) * CELL_XZ:.0f},{(cells[c] + 1) * CELL_Y + EYE:.0f},-10,{yaw:.0f}")
    print(";".join(poses))
    sys.exit()
for k in range(count):
    angle = 2 * math.pi * k / count + math.pi / 4
    d = np.array([math.cos(angle), math.sin(angle)])
    # The floor cell furthest along d, pulled a quarter of the way back to the center.
    far = pts[np.argmax(pts @ d)]
    at = far + (center - far) * 0.25 + 0.5
    look = center + 0.5 - at
    yaw = math.degrees(math.atan2(look[1], look[0]))
    poses.append(f"{at[0] * CELL_XZ:.0f},{at[1] * CELL_XZ:.0f},{(floor + 1) * CELL_Y + EYE:.0f},-10,{yaw:.0f}")
print(";".join(poses))
