#!/usr/bin/env python3
"""Fix the GBA ROM header checksum and pad the ROM. For real hardware, run
devkitPro's `gbafix` instead (it also inserts the boot logo the BIOS checks)."""
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
c = 0
for b in d[0xA0:0xBD]:
    c -= b
d[0xBD] = (c - 0x19) & 0xFF
while len(d) % 256:
    d.append(0xFF)
open(p, 'wb').write(d)
