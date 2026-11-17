#!/usr/bin/env python3
"""Lay out the run stage: hover-car encounters, hover pads and orbs, with a
gentle difficulty ramp. Writes src/gen/level_gen.[ch]."""
import os, sys, random
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from gfxlib import CWriter

GROUND_Y = 140
LEVEL_LEN = 10240
random.seed(1996)

BOLT, ORCA, WASP, MANTA = range(4)
cars = []        # (trigger cam x, type, speed q8, y_off)
pads = []        # (x, y_top, width_tiles) ; x, y multiples of 8
orbs = []        # (x, y)


def orb_line(x, y, n, dx=18):
    for i in range(n):
        orbs.append((x + i * dx, y))


def orb_arc(x, y, n, h=36, dx=18):
    for i in range(n):
        t = i / (n - 1) if n > 1 else 0.5
        orbs.append((x + i * dx, int(y - h * 4 * t * (1 - t))))


def pad(x, y, w):
    pads.append((x // 8 * 8, y // 8 * 8, w))


x = 480
orb_line(260, GROUND_Y - 12, 5)
section = 0
while x < LEVEL_LEN - 900:
    prog = x / LEVEL_LEN
    r = random.random()
    speed = int((1.6 + 1.6 * prog + random.random() * 0.6) * 256)
    if r < 0.28:
        cars.append((x, BOLT, speed, 0))
        orb_arc(x + 120, GROUND_Y - 30, 5, 30)
        x += random.randint(300, 420) - int(120 * prog)
    elif r < 0.45:
        cars.append((x, ORCA, int(speed * 0.8), 0))
        orb_line(x + 160, GROUND_Y - 10, 4)
        x += random.randint(320, 440) - int(120 * prog)
    elif r < 0.58 and prog > 0.15:
        cars.append((x, WASP, speed, random.choice((0, 1))))
        x += random.randint(300, 400) - int(100 * prog)
    elif r < 0.7 and prog > 0.3:
        cars.append((x, MANTA, int(speed * 1.35), 0))
        orb_arc(x + 100, GROUND_Y - 34, 4, 26)
        x += random.randint(320, 420) - int(100 * prog)
    elif r < 0.85:
        # hover-pad staircase with an orb trail: the safe high road over a car
        px = x + 40
        pad(px, GROUND_Y - 40, 6)
        orb_line(px + 6, GROUND_Y - 56, 3)
        pad(px + 96, GROUND_Y - 72, 6)
        orb_arc(px + 100, GROUND_Y - 92, 4, 20)
        pad(px + 200, GROUND_Y - 48, 5)
        cars.append((x + 60, random.choice((BOLT, BOLT, MANTA if prog > 0.3 else BOLT)), speed, 0))
        x += 360
    else:
        # double trouble: low then high
        cars.append((x, BOLT, speed, 0))
        cars.append((x + 70, ORCA, int(speed * 0.8), 0))
        orb_line(x + 200, GROUND_Y - 12, 3)
        x += random.randint(380, 480)
    section += 1

# final stretch: an orb runway to the car
orb_line(LEVEL_LEN - 700, GROUND_Y - 12, 14, 24)

CHECKPOINTS = [0, 2560, 5120, 7680]


def main():
    cars.sort(); pads.sort(); orbs.sort()
    out = CWriter(os.path.join(ROOT, 'src/gen/level_gen.c'), os.path.join(ROOT, 'src/gen/level_gen.h'), 'tools/gen_level.py')
    out.define('LEVEL_LEN', LEVEL_LEN)
    out.define('GROUND_Y', GROUND_Y)
    out.define('NUM_CARS', len(cars))
    out.define('NUM_PADS', len(pads))
    out.define('NUM_ORBS', len(orbs))
    out.define('NUM_CHECKPOINTS', len(CHECKPOINTS))
    out.arr('s16', 'lvl_car', [v for c in cars for v in (c[0], c[1], c[2], c[3])], 16, '%d')
    out.arr('s16', 'lvl_pad', [v for p in pads for v in (p[0], p[1], p[2])], 15, '%d')
    out.arr('s16', 'lvl_orb', [v for o in orbs for v in (o[0], o[1])], 16, '%d')
    out.arr('s16', 'lvl_check', CHECKPOINTS, 8, '%d')
    out.close()
    print('cars', len(cars), 'pads', len(pads), 'orbs', len(orbs))


if __name__ == '__main__':
    main()
