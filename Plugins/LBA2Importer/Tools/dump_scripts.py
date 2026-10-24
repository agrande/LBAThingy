"""Disassembles the track (move) and life scripts of LBA2 scene objects, using the opcode tables of the
lba2remake checkout (src/game/scripting/data/lba2). Usage: dump_scripts.py scene [object ...] [-track] [-life]"""
import os, re, struct, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0])
from dump_scenes import parse

TOOLS = os.path.dirname(os.path.abspath(__file__))
REMAKE = os.environ.get("LBA2_REMAKE", os.path.join(TOOLS, "../../../../refs/lba2remake"))
DATA = os.path.join(REMAKE, "src/game/scripting/data/lba2")
SIZES = {"Int8": "b", "Uint8": "B", "Int16": "h", "Uint16": "H", "Int32": "i", "Uint32": "I"}


def load_table(name):
    """{opcode: dict(command, args, condition, operator, argsFirst, precond, param, operand)} from a .ts table."""
    text = open(os.path.join(DATA, name), encoding="utf-8").read()
    table = {}
    for block in re.findall(r"\{\s*opcode:(.*?)\n\s*\}", text, re.S):
        code = int(re.match(r"\s*(0x[0-9A-Fa-f]+|\d+)", block).group(1), 0)
        e = {"command": re.search(r"command:\s*'([^']*)'", block).group(1)}
        m = re.search(r"args:\s*\[([^\]]*)\]", block)
        e["args"] = re.findall(r"'([^']*)'", m.group(1)) if m else []
        for key in ("condition", "operator", "argsFirst", "precond"):
            e[key] = re.search(key + r":\s*true", block) is not None
        for key in ("param", "operand"):
            m = re.search(key + r":\s*'([^']*)'", block)
            e[key] = m.group(1) if m else None
        table[code] = e
    return table


MOVE, LIFE, COND = load_table("move.ts"), load_table("life.ts"), load_table("condition.ts")
OPS = ["==", ">", "<", ">=", "<=", "!="]


def value(data, at, spec):
    t = spec.split(":")[0].lstrip("_")
    if t == "string":
        end = data.index(0, at)
        return data[at:end].decode("latin-1"), end + 1
    f = "<" + SIZES[t]
    return struct.unpack_from(f, data, at)[0], at + struct.calcsize(f)


def disassemble(data, table):
    """[(offset, text)]"""
    out, at, switch_cond = [], 0, None
    while at < len(data):
        start = at
        op = table.get(data[at]); at += 1
        if op is None:
            out.append((start, "?? 0x%02X" % data[start])); break
        parts = [op["command"]]
        def args():
            nonlocal at
            for spec in op["args"]:
                v, at = value(data, at, spec)
                parts.append(str(v))
            if op["command"] in ("SET_DIRMODE", "SET_DIRMODE_OBJ") and parts and int(parts[-1]) in (2, 4, 6, 9, 10, 11):
                parts.append(str(data[at])); at += 1
        if op["argsFirst"]:
            args()
        cond = None
        if op["condition"]:
            cond = COND[data[at]]; at += 1
            parts.append(cond["command"])
            if cond["param"]:
                v, at = value(data, at, cond["param"]); parts.append("(%s)" % v)
            if op["command"] == "SWITCH":
                switch_cond = cond
        elif op["operator"]:
            cond = switch_cond
        if op["operator"]:
            parts.append(OPS[data[at]]); at += 1
            v, at = value(data, at, cond["operand"]); parts.append(str(v))
        if not op["argsFirst"]:
            args()
        out.append((start, " ".join(parts)))
        if op["command"] == "END" and table is MOVE:
            pass
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    which = [w for w in ("track", "life") if "-" + w in sys.argv] or ["track", "life"]
    scene = parse(int(args[0]))
    objects = [int(a) for a in args[1:]] or range(1, len(scene["actors"]) + 1)
    for i in objects:
        a = scene["actors"][i - 1]
        print("=== A%02d sprite=%d file3d=%d flags=%08X" % (i, a["sprite"], a["file3d"], a["flags"]))
        for w in which:
            print("  --", w)
            for at, text in disassemble(a[w], MOVE if w == "track" else LIFE):
                print("  %4d  %s" % (at, text))
