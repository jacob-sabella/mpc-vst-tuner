#!/usr/bin/env python3
"""Draw canvas.png, the area the skin leaves for screendraw.c: a dark box whose left and right edge columns alternate
two marker colours each (so the drawing thread can find it on screen), with a line along the top and bottom.

    make_canvas.py [width height [out.png]]   (default 960 440 vst/canvas.png; keep the height even: the last marker
                                               row is then colour b)
"""
import re, sys, zlib, struct, pathlib

here = pathlib.Path(__file__).parent
defs = dict((k, int(v, 16)) for k, v in re.findall(r"#define (SCR_\w+) 0x([0-9A-Fa-f]{6})", (here.parent / "src" / "screendraw.h").read_text()))
w, h = (int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) >= 3 else (960, 440)
out = pathlib.Path(sys.argv[3]) if len(sys.argv) >= 4 else here.parent / "vst" / "canvas.png"

def rgb(c): return bytes((c >> 16 & 255, c >> 8 & 255, c & 255))

rows = []
for y in range(h):
    line = rgb(defs["SCR_CANVAS_LINE"] if y in (0, h - 1) else defs["SCR_CANVAS_BG"]) * w
    left = rgb(defs["SCR_MARK_L_B" if y & 1 else "SCR_MARK_L_A"])
    right = rgb(defs["SCR_MARK_R_B" if y & 1 else "SCR_MARK_R_A"])
    rows.append(b"\0" + left + line[3:-3] + right)

def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
out.write_bytes(png)
print("%s %dx%d" % (out, w, h))
