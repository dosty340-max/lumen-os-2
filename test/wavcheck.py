#!/usr/bin/env python3
"""Report peak level per second of a QEMU wav capture (header may be unfinished while running)."""
import struct, sys
d = open(sys.argv[1], "rb").read()[44:]
n = len(d) // 4
fr = struct.unpack("<%dh" % (n * 2), d[:n * 4])
rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
peaks = [max(abs(v) for v in fr[i:i + rate * 2]) if fr[i:i + rate * 2] else 0 for i in range(0, len(fr), rate * 2)]
print("seconds:", n / rate, "peaks:", peaks)
print("NONSILENT" if max(peaks or [0]) > 500 else "SILENT")
