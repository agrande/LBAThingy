"""Dumps LBA2 SCENE.HQR scenes (header, actors, zones, points). Usage: dump_scenes.py [scene ...] [-v]"""
import os, struct, sys, collections
sys.path.insert(0, __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0])
from hqr import entry

# The game's data folder: LBA2_GAME, else the GOG install.
GAME = os.environ.get("LBA2_GAME", "C:/GOG Games/Little Big Adventure 2").rstrip("/\\") + "/"
ISLANDS = ["citadel", "sendell", "desert", "emeraude", "otringal", "celebrat", "platform", "mosquibe",
           "knartas", "ilotcx", "ascence", "souscelb"]


def parse(i):
    d = entry(GAME + "SCENE.HQR", i + 1)
    o = 0
    def g(fmt):
        nonlocal o
        v = struct.unpack_from("<" + fmt, d, o); o += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]
    s = {}
    s["island"], s["cubex"], s["cubey"], s["shadow"], s["labyrinth"], s["mode"], _ = g("bbbbbbb")
    s["alpha"], s["beta"] = g("hh")
    s["samples"] = [g("hhhhh") for _ in range(4)]
    s["secmin"], s["secrnd"] = g("hh")
    s["jingle"] = g("b")
    s["hero"] = g("hhh")
    n = g("h"); o += n; n = g("h"); o += n
    actors = []
    nobj = g("h")
    for a in range(1, nobj):
        flags = g("I"); f3d = g("h"); body = g("b"); anim = g("h"); spr = g("h"); pos = g("hhh")
        hit = g("b"); opt = g("h"); beta = g("h"); srot = g("h"); move = g("b"); info = g("hhhh")
        bonus = g("h"); col = g("b")
        if flags & (1 << 18):
            g("I"); g("h")
        armour, life = g("bb")
        ms = g("h"); o += ms; ls = g("h"); o += ls
        actors.append(dict(flags=flags, file3d=f3d, body=body, anim=anim, sprite=spr, pos=pos, beta=beta,
                           move=move, info=info, move_size=ms, life_size=ls))
    g("I")
    zones = []
    nz = g("h")
    for z in range(nz):
        box = g("iiiiii"); info = g("iiiiiiii"); typ, num = g("hh")
        zones.append(dict(box=box, info=info, type=typ, num=num))
    pts = [g("iii") for _ in range(g("h"))]
    s.update(actors=actors, zones=zones, points=pts, size=len(d), end=o)
    return s


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    scenes = [int(a) for a in args] or range(222)
    ztypes = collections.Counter(); sprites = 0; total = 0
    for i in scenes:
        try:
            s = parse(i)
        except Exception as e:
            print(i, "ERR", e); continue
        isl = ISLANDS[s["island"]] if 0 <= s["island"] < len(ISLANDS) else s["island"]
        nspr = sum(1 for a in s["actors"] if a["flags"] & (1 << 10))
        sprites += nspr; total += len(s["actors"])
        for z in s["zones"]:
            ztypes[z["type"]] += 1
        print(f"{i:3d} {isl:9s} cube=({s['cubex']},{s['cubey']}) mode={s['mode']} hero={s['hero']} "
              f"actors={len(s['actors'])} sprites={nspr} zones={len(s['zones'])} points={len(s['points'])} "
              f"amb={[x[0] for x in s['samples']]} jingle={s['jingle']} left={s['size'] - s['end']}")
        if verbose:
            for n, a in enumerate(s["actors"], 1):
                print(f"   A{n:02d} flags={a['flags']:08x} f3d={a['file3d']} body={a['body']} anim={a['anim']} spr={a['sprite']} "
                      f"pos={a['pos']} beta={a['beta']} move={a['move']} info={a['info']} track={a['move_size']} life={a['life_size']}")
            for n, z in enumerate(s["zones"]):
                print(f"   Z{n:02d} type={z['type']} num={z['num']} box={z['box']} info={z['info']}")
            for n, p in enumerate(s["points"]):
                print(f"   P{n:02d} {p}")
    print("actors", total, "sprites", sprites, "zone types", sorted(ztypes.items()))
