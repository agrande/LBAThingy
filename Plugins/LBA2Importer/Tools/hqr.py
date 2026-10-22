import struct

def entry(path, index):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, index * 4)[0]
    size, csize, method = struct.unpack_from("<IIH", d, off)
    src = d[off + 10: off + 10 + csize]
    if method == 0:
        return src[:size]
    out = bytearray(); i = 0; mm = method + 1
    while len(out) < size and i < len(src):
        flags = src[i]; i += 1
        for bit in range(8):
            if len(out) >= size: break
            if flags & (1 << bit):
                out.append(src[i]); i += 1
            else:
                w = src[i] | (src[i + 1] << 8); i += 2
                n = (w & 15) + mm; f = len(out) - ((w >> 4) + 1)
                for _ in range(n):
                    out.append(out[f] if f >= 0 else 0); f += 1
    return bytes(out)

