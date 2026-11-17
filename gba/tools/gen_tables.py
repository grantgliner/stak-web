#!/usr/bin/env python3
"""Generate math lookup tables (sin LUT) for the GBA build."""
import math, os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
with open(os.path.join(ROOT, 'src/gen/tables_gen.h'), 'w') as f:
    f.write('#pragma once\n#include "../gba.h"\nextern const s32 sin_lut[1024];\n')
with open(os.path.join(ROOT, 'src/gen/tables_gen.c'), 'w') as f:
    f.write('#include "tables_gen.h"\nconst s32 sin_lut[1024] = {\n')
    v = [int(round(math.sin(2 * math.pi * i / 1024) * 65536)) for i in range(1024)]
    for i in range(0, 1024, 16):
        f.write('  ' + ','.join(map(str, v[i:i + 16])) + ',\n')
    f.write('};\n')
