#!/usr/bin/env python3
"""Drive-stage (Mode 7) assets: the circuit as a 1024x1024 8bpp affine map,
its collision/progress data, the horizon panorama, and the rear-view cars.

The track is a closed path on a 32 px grid using only 0/45/90 degree moves.
The road is the union of squares swept along that path (an L-infinity
distance band), so every edge runs at 0, 45 or 90 degrees on an 8 px-aligned
line, which keeps the unique tile count under the affine BG's 256-tile limit.
"""
import os, sys, math
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from gfxlib import *
import gen_run

MAP = 1024
HALF = 48          # road half-width (L-inf), tile aligned
RAIL = 8           # barrier band
rng = np.random.default_rng(5)

# closed path on the 32 px grid (cell coordinates), drawn anticlockwise-ish:
# a long start straight, a sweeping hairpin, an S-bend and a diagonal blast
PATH = [(4, 26), (4, 12), (7, 6), (12, 4), (18, 4), (22, 8), (22, 12), (18, 16), (18, 20), (22, 24),
        (26, 24), (28, 20), (28, 8), (26, 5), (24, 3)]
# rebuild as a tidy loop that only uses 0/45/90 degree legs
PATH = [(4, 27), (4, 10), (8, 6), (14, 6), (17, 9), (17, 13), (13, 17), (13, 20), (16, 23), (22, 23),
        (25, 20), (25, 8), (28, 5), (28, 4)]
PATH = [(4, 27), (4, 9), (8, 5), (15, 5), (18, 8), (18, 12), (14, 16), (14, 19), (17, 22), (23, 22),
        (26, 19), (26, 7), (29, 4)]


def loop_path():
    pts = [(4, 26), (4, 8), (8, 4), (24, 4), (28, 8), (28, 14), (24, 18), (18, 18), (14, 22), (14, 26), (12, 28), (6, 28)]
    return [(x * 32, y * 32) for x, y in pts]


def check_legs(pts):
    for (a, b) in zip(pts, pts[1:] + pts[:1]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        assert dx == 0 or dy == 0 or abs(dx) == abs(dy), (a, b)


def sample_path(pts, step=1.0):
    out = []
    for (a, b) in zip(pts, pts[1:] + pts[:1]):
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        n = max(1, int(L / step))
        for i in range(n):
            t = i / n
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    return out


def sweep_mask(samples, half):
    m = np.zeros((MAP, MAP), bool)
    for (x, y) in samples[::2]:
        x0, y0 = int(round(x - half)), int(round(y - half))
        m[max(0, y0):max(0, y0 + 2 * half), max(0, x0):max(0, x0 + 2 * half)] = True
    return m


# palette (256, but we only use a few dozen)
PAL = {
    'sea0': '#5a1e6e', 'sea1': '#6c2a80', 'sea2': '#8a3c96', 'sea3': '#c86ab4', 'sea4': '#ffd0ec',
    'road0': '#e9d6ee', 'road1': '#dcc4e6', 'road2': '#f6eaf6', 'roadline': '#ffffff',
    'rail0': '#ff4aa6', 'rail1': '#58e4ff', 'rail2': '#ffffff', 'raildark': '#3a1050',
    'boost0': '#ffd24a', 'boost1': '#fff6b0', 'boost2': '#ff8a2c',
    'chk0': '#1c0a2c', 'chk1': '#ffffff', 'edge': '#b494c8',
}
NAMES = list(PAL)
IDX = {k: i + 1 for i, k in enumerate(NAMES)}


def build_track():
    pts = loop_path()
    check_legs(pts)
    samples = sample_path(pts, 1.0)
    road = sweep_mask(samples, HALF)
    rail = sweep_mask(samples, HALF + RAIL) & ~road
    img = np.zeros((MAP, MAP), np.uint8)
    yy, xx = np.mgrid[0:MAP, 0:MAP]
    # sea: waves pattern on an 8 px grid with a few tile variants
    var = rng.choice([0, 0, 0, 1, 2, 3], (MAP // 8, MAP // 8))
    tiles = []
    for v in range(6):
        t = np.full((8, 8), IDX['sea0'], np.uint8)
        if v == 1:
            t[3, 1:4] = IDX['sea2']; t[2, 4] = IDX['sea2']
        elif v == 2:
            t[5, 3:7] = IDX['sea1']
        elif v == 3:
            t[2, 5] = IDX['sea4']; t[1, 5] = IDX['sea3']; t[2, 4] = IDX['sea3']; t[3, 5] = IDX['sea3']; t[2, 6] = IDX['sea3']
        elif v == 4:
            t[6, 0:3] = IDX['sea2']; t[5, 3] = IDX['sea2']
        tiles.append(t)
    for ty in range(MAP // 8):
        for tx in range(MAP // 8):
            img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = tiles[var[ty, tx]]
    # road surface: soft pearl checker on a 32 px period, to read speed
    chk = ((xx // 16 + yy // 16) % 2).astype(bool)
    img[road] = np.where(chk[road], IDX['road0'], IDX['road1'])
    # light edge strip just inside the barrier
    inner = sweep_mask(samples, HALF - 8)
    img[road & ~inner] = IDX['road2']
    # barrier: alternating pink/cyan blocks with a bright core line
    blk = (((xx // 8) + (yy // 8)) % 4 < 2)
    img[rail] = np.where(blk[rail], IDX['rail0'], IDX['rail1'])
    core = sweep_mask(samples, HALF + 4) & rail & ~sweep_mask(samples, HALF + 3)
    img[core] = IDX['rail2']
    outer_line = sweep_mask(samples, HALF + RAIL) & ~sweep_mask(samples, HALF + RAIL - 1)
    img[outer_line] = IDX['raildark']
    # start/finish checker band across the first (vertical) leg
    sx, sy = pts[0][0], pts[0][1] - 64
    for y in range(sy - 8, sy + 8):
        for x in range(sx - HALF, sx + HALF):
            img[y, x] = IDX['chk0'] if ((x // 8) + (y // 8)) % 2 else IDX['chk1']
    # boost pads (chevrons pointing along travel), on straights
    boosts = []
    def chevrons(cx, cy, d):
        # d: direction of travel (dx, dy) axis-aligned; draw 32x32 pad
        for j in range(32):
            for i in range(32):
                # local coords: u along travel, v across
                u, v = j, i - 16
                on = ((abs(u - 16) + abs(v)) % 8) < 3
                x, y = (cx + v, cy - u) if d == (0, -1) else (cx + v, cy + u) if d == (0, 1) else (cx + u, cy + v) if d == (1, 0) else (cx - u, cy + v)
                img[y, x] = IDX['boost0'] if on else IDX['boost2']
                if on and ((abs(u - 16) + abs(v)) % 8) == 0:
                    img[y, x] = IDX['boost1']
        boosts.append((cx // 8, cy // 8))
    chevrons(4 * 32, 16 * 32, (0, -1))
    chevrons(16 * 32, 4 * 32, (1, 0))
    chevrons(28 * 32, 10 * 32, (0, 1))
    chevrons(21 * 32, 18 * 32, (-1, 0))
    return pts, samples, road, rail, img


def tile_map(img):
    tiles, entries = [], []
    lut = {}
    tiles.append(np.zeros((8, 8), np.uint8)); lut[tiles[0].tobytes()] = 0
    for ty in range(MAP // 8):
        for tx in range(MAP // 8):
            t = img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
            k = t.tobytes()
            if k not in lut:
                lut[k] = len(tiles); tiles.append(t.copy())
            entries.append(lut[k])
    return tiles, entries


# ------------------------------------------------------------------ horizon panorama (BG1, 4bpp, 512 x 64 visible band)
PAL_HZ = ['#000000', '#b07cc0', '#c494cc', '#d6acd6', '#e6c4e0', '#f2d8ea', '#fff4f8', '#fcd8e6', '#eab4d2',
          '#a8f0ff', '#f6d0e2', '#fff6dc', '#9a68b4', '#ffe8f0', '#7a4ca0', '#ffffff']
HZ = 56   # horizon line on screen


def horizon():
    W, Hh = 512, 72
    c = np.zeros((Hh, W), np.uint8)
    far = gen_run.far_layer()           # reuse the run stage's skyline style
    # tile the 256-wide skyline twice, aligned so its base sits at the horizon
    base = 116
    for x in range(W):
        for y in range(Hh):
            sy = base - (HZ - y) if y < HZ else None
            if sy is not None and 0 <= sy < 160:
                v = far[sy, x % 256]
                if v and v != 10:
                    c[y, x] = v
    # haze band at and just under the horizon
    for y in range(HZ - 3, HZ + 8):
        for x in range(W):
            if y >= HZ or c[y, x] == 0:
                c[y, x] = 13 if y < HZ + 4 else (13 if (x + y) % 2 else 0)
    # a big pearl planet with ring
    ins, v, sp = gen_run.sphere_shade(40, 40, (-0.4, -0.5))
    idx = dither_ramp(v, [8, 7, 6], 0, 0)
    for y in range(40):
        for x in range(40):
            if ins[y, x]:
                c[4 + y, 300 + x] = 6 if sp[y, x] > 0.6 else idx[y, x]
    for t in np.linspace(0, 2 * math.pi, 600):
        x = 320 + math.cos(t) * 52; y = 24 + math.sin(t) * 9
        if not (math.sin(t) < 0 and abs(math.cos(t)) < 0.55):
            c[int(round(y)), int(round(x)) % W] = 9 if math.sin(t) > 0 else 4
    return c


SKY_KEYS = [(0, '#7a2e94'), (24, '#b04aa6'), (44, '#f07ab0'), (56, '#ffc8c8'), (57, '#5a1e6e'), (160, '#5a1e6e')]


# ------------------------------------------------------------------ rear-view cars
REAR_PAL = ['#000000', '#1c0a2c', '#c0407c', '#ff76aa', '#ffa8c8', '#ffd8e8', '#ffffff', '#58e4ff', '#c8faff',
            '#ffd24a', '#3a2418', '#22140c', '#9a6438', '#6a4020', '#3a1050', '#ff4aa6']


def player_rear(bank):
    """64x32: the Twin Turbo from behind, both riders visible. bank -1/0/1"""
    c = np.zeros((32, 64), np.uint8)
    sh = bank  # top parts shift sideways with the lean
    # body: wide rounded wedge
    for y in range(12, 28):
        t = (y - 12) / 16
        hw = int(22 + 8 * math.sin(min(1, t * 1.6) * math.pi / 2))
        off = int(round(sh * (1 - t) * 3))
        for x in range(32 - hw + off, 32 + hw + off):
            if 0 <= x < 64:
                col = [5, 4, 4, 3, 3, 3, 2, 2][min(7, int(t * 8))]
                c[y, x] = col
    # spoiler
    for x in range(10, 54):
        c[10, x + sh * 2] = 3; c[11, x + sh * 2] = 2
    for x in (14, 15, 48, 49):
        c[11, x + sh * 2] = 14; c[12, x + sh * 2] = 14
    # riders' heads (left seat Franco: dark hair, right seat tripleset: wavy light brown)
    for (cx, hair, dark) in ((25, 10, 11), (39, 12, 13)):
        cx += sh * 3
        for y in range(2, 11):
            for x in range(cx - 5, cx + 5):
                d = math.hypot(x + 0.5 - cx, (y - 6.5) * 1.15)
                if d < 5:
                    c[y, x] = hair if d < 3.5 or (x + y) % 3 else dark
        if hair == 12:
            for x in range(cx - 5, cx + 5, 2):
                c[2, x] = 0 if c[2, x] == 0 else 12
        c[9, cx - 1:cx + 1] = 4  # neck
    # twin thrusters
    for (cx, cy) in ((20, 21), (44, 21)):
        cx += sh
        for y in range(cy - 5, cy + 5):
            for x in range(cx - 6, cx + 6):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if d < 5.5:
                    c[y, x] = 1 if d > 4.5 else (8 if d < 2 else 7)
    # tail light strip + highlight
    for x in range(14, 50):
        c[15, x] = 15 if 26 < x < 38 else 6
    c[13, 18:46] = 5
    # under-glow
    for x in range(12, 52, 2):
        c[30, x] = 7
    m = c > 0
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]; nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    c[(~m) & nb] = 1
    return c


def rival_rear(flame):
    """32x32 rear view in the run-stage car palettes (slots match CAR_PALS)"""
    c = np.zeros((32, 32), np.uint8)
    for y in range(12, 26):
        t = (y - 12) / 14
        hw = int(10 + 4 * math.sin(min(1, t * 1.7) * math.pi / 2))
        for x in range(16 - hw, 16 + hw):
            c[y, x] = [5, 4, 4, 3, 3, 2, 2][min(6, int(t * 7))]
    # canopy bubble
    for y in range(6, 13):
        for x in range(10, 22):
            if math.hypot((x + 0.5 - 16) / 6, (y + 0.5 - 12) / 6) < 1:
                c[y, x] = 11 if y > 8 else 10
    c[8, 13:15] = 12
    # stripe and thrusters
    c[17, 6:26] = 7
    for cx in (10, 22):
        for y in range(19, 25):
            for x in range(cx - 3, cx + 3):
                if math.hypot(x + 0.5 - cx, y + 0.5 - 22) < 3:
                    c[y, x] = 9 if flame else 8
    for x in range(8, 24, 2):
        c[27, x] = 13
    m = c > 0
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]; nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    c[(~m) & nb] = 1
    return c


def main():
    pts, samples, road, rail, img = build_track()
    tiles, entries = tile_map(img)
    print('track tiles', len(tiles))
    assert len(tiles) <= 256, len(tiles)
    out = CWriter(os.path.join(ROOT, 'src/gen/drive_gen.c'), os.path.join(ROOT, 'src/gen/drive_gen.h'), 'tools/gen_drive.py')
    pal = [0] + [rgb15(hexc(PAL[k])) for k in NAMES]
    pal += [0] * (256 - len(pal))
    out.u16('track_pal', pal)
    words = []
    for t in tiles:
        words += tile8(t)
    out.u32('track_tiles', words)
    # affine map: bytes
    out.u8('track_map', entries)
    # collision grid at 8 px: 0 sea, 1 road, 2 rail, 3 boost
    col = []
    for ty in range(MAP // 8):
        for tx in range(MAP // 8):
            r = road[ty * 8 + 4, tx * 8 + 4]
            rl = rail[ty * 8 + 4, tx * 8 + 4]
            v = img[ty * 8 + 4, tx * 8 + 4]
            col.append(3 if v in (IDX['boost0'], IDX['boost2'], IDX['boost1']) else 1 if r else 2 if rl else 0)
    out.u8('track_col', col)
    # centreline waypoints every 16 px (progress, AI, respawn)
    wp = sample_path(pts, 16.0)
    out.define('NUM_WP', len(wp))
    out.arr('s16', 'track_wp', [int(round(v)) for p in wp for v in p], 16, '%d')
    out.define('TRACK_HALF', HALF)
    # horizon
    hz = horizon()
    t2, e2 = tiles_dedupe(hz, 4, flip=True)
    out.u32('hz_tiles', sum(t2, []))
    m2 = np.array(e2).reshape(hz.shape[0] // 8, 64)
    out.u16('hz_map', list(m2[:, :32].reshape(-1)) + list(m2[:, 32:].reshape(-1)))
    out.define('HZ_ROWS', hz.shape[0] // 8)
    out.u16('hz_pal', pal15(PAL_HZ))
    out.define('HORIZON', HZ)
    out.u16('drive_sky', gen_run.sky_table(SKY_KEYS, 160))
    print('horizon tiles', len(t2))
    # cars
    words = []
    for b in (-1, 0, 1):
        words += obj_tiles(player_rear(b), 8, 4)
    out.u32('rear_tiles', words)
    out.u16('rear_pal', pal15(REAR_PAL))
    words = []
    for f in range(2):
        words += obj_tiles(rival_rear(f), 4, 4)
    out.u32('rival_tiles', words)
    out.close()
    # previews
    P = [(0, 0, 0)] + [hexc(PAL[k]) for k in NAMES]
    rgb = np.array(P, np.uint8)[img]
    Image.fromarray(rgb).resize((512, 512), Image.NEAREST).save(os.path.join(ROOT, 'build/track.png'))
    sheet = np.zeros((32, 64 * 3 + 64, 3), np.uint8); sheet[:] = (90, 30, 110)
    RP = [hexc(c) for c in REAR_PAL]
    for i, b in enumerate((-1, 0, 1)):
        a = player_rear(b)
        for y in range(32):
            for x in range(64):
                if a[y, x]:
                    sheet[y, i * 64 + x] = RP[a[y, x]]
    a = rival_rear(1); CP = [hexc(c) for c in gen_run.CAR_PALS['bolt']]
    for y in range(32):
        for x in range(32):
            if a[y, x]:
                sheet[y, 192 + x] = CP[a[y, x]]
    Image.fromarray(sheet).resize((sheet.shape[1] * 3, 96), Image.NEAREST).save(os.path.join(ROOT, 'build/rear.png'))


if __name__ == '__main__':
    main()
