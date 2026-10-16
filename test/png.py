"""Minimal PNG reader (stdlib only) for test assertions on QEMU screendumps."""
import struct, zlib

def read(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    i, idat, w = 8, b"", 0
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        if t == b"IHDR": w, h, depth, ctype = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT": idat += c
        i += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows

def pixel(img, x, y):
    w, h, bpp, rows = img
    return tuple(rows[y][x * bpp:x * bpp + 3])

def diff(a, b, step=7):
    """fraction of sampled pixels that differ noticeably"""
    A, B = read(a), read(b)
    n = d = 0
    for y in range(0, A[1], step):
        for x in range(0, A[0], step):
            n += 1
            if sum(abs(p - q) for p, q in zip(pixel(A, x, y), pixel(B, x, y))) > 30: d += 1
    return d / n
