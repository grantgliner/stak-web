#!/usr/bin/env python3
"""Run-stage art: three parallax background layers in a glossy Y2K
retro-future style (pearl-pink chrome bubbles, cyan glints, taken from the
TURBO EP cover), the road and hover pads, the oncoming hover cars, orbs and
small effects. Writes src/gen/run_gen.[ch] and preview PNGs in build/."""
import os, sys, math
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from gfxlib import *

rng = np.random.default_rng(3)
PREV = os.path.join(ROOT, 'build')
os.makedirs(PREV, exist_ok=True)

# ------------------------------------------------------------------ palettes
PAL_FAR = ['#000000', '#b07cc0', '#c494cc', '#d6acd6', '#e6c4e0', '#f2d8ea', '#fff4f8', '#fcd8e6', '#eab4d2',
           '#a8f0ff', '#f6d0e2', '#fff6dc', '#9a68b4', '#000000', '#000000', '#000000']
PAL_MID = ['#000000', '#3a1e52', '#74387e', '#a04c96', '#cc6aac', '#ee92c2', '#ffc2dc', '#fff2f8', '#40c0e4',
           '#b0f4ff', '#fff0a8', '#8c6cb4', '#ffffff', '#ff4aa6', '#5c3a80', '#d6b4e6']
PAL_ROAD = ['#000000', '#1c0a2c', '#36164a', '#45205e', '#552a70', '#f6e6f6', '#ff76b2', '#ffbedc', '#58e4ff',
            '#c8faff', '#c03ca0', '#8a6ab0', '#e4d4f6', '#ffffff', '#2a1040', '#ff9ec8']
PAL_HUD = ['#000000', '#1c0a2c', '#ffffff', '#fff07a', '#ff8cc0', '#8cf0ff', '#6a4a8a', '#ffd0e4',
           '#000000', '#000000', '#000000', '#000000', '#000000', '#000000', '#000000', '#000000']

# sky gradient (one colour per scanline, 160 lines + spare)
SKY_KEYS = [(0, '#8a3e9e'), (40, '#c25aa8'), (80, '#f08ab6'), (108, '#ffc2c4'), (124, '#ffe6d2'), (160, '#ffe6d2')]


def grad(keys, n):
    out = []
    for y in range(n):
        for (y0, c0), (y1, c1) in zip(keys, keys[1:]):
            if y0 <= y <= y1:
                t = 0 if y1 == y0 else (y - y0) / (y1 - y0)
                a, b = np.array(hexc(c0)), np.array(hexc(c1))
                out.append(tuple(a + (b - a) * t))
                break
    return out


def sky_table(keys, n=160):
    cols = grad(keys, n)
    # 15-bit with a little vertical dithering between neighbouring 5-bit levels
    out = []
    for y, c in enumerate(cols):
        r, g, b = [v / 255 * 31 for v in c]
        th = (0.25, 0.75)[y & 1]
        q = [int(math.floor(v + th)) for v in (r, g, b)]
        q = [max(0, min(31, v)) for v in q]
        out.append(q[0] | q[1] << 5 | q[2] << 10)
    return out


# ------------------------------------------------------------------ shading helpers
def sphere_shade(h, w, light=(-0.55, -0.6)):
    yy, xx = np.mgrid[0:h, 0:w]
    nx = (xx + 0.5 - w / 2) / (w / 2)
    ny = (yy + 0.5 - h / 2) / (h / 2)
    r2 = nx * nx + ny * ny
    inside = r2 <= 1
    nz = np.sqrt(np.clip(1 - r2, 0, 1))
    lx, ly = light
    lz = math.sqrt(max(0, 1 - lx * lx - ly * ly))
    d = np.clip(nx * lx + ny * ly + nz * lz, 0, 1)
    spec = np.clip(d, 0, 1) ** 18
    return inside, d * 0.85 + 0.1, spec


def capsule_shade(h, w, light=-0.6):
    """vertical capsule / cylinder: horizontal shading across, rounded caps"""
    yy, xx = np.mgrid[0:h, 0:w]
    nx = (xx + 0.5 - w / 2) / (w / 2)
    r = w / 2
    inside = np.ones((h, w), bool)
    # rounded top
    top = yy < r
    inside &= ~top | ((xx + 0.5 - r) ** 2 + (yy + 0.5 - r) ** 2 <= r * r)
    nz = np.sqrt(np.clip(1 - nx * nx, 0, 1))
    d = np.clip(nx * light + nz * 0.8, 0, 1)
    spec = np.clip(d, 0, 1) ** 14
    return inside, d * 0.85 + 0.1, spec


def stamp(canvas, x0, y0, mask, idx):
    h, w = mask.shape
    H, W = canvas.shape
    for y in range(h):
        yy = y0 + y
        if not (0 <= yy < H):
            continue
        for x in range(w):
            xx = (x0 + x) % W
            if mask[y, x]:
                canvas[yy, xx] = idx[y, x]


def shade_obj(canvas, x0, y0, inside, val, spec, ramp, spec_idx=None, spec_th=0.6):
    idx = dither_ramp(val, ramp, x0, y0)
    if spec_idx is not None:
        idx = np.where(spec > spec_th, spec_idx, idx)
    stamp(canvas, x0, y0, inside, idx)


def outline_mask(canvas, color, where_bg=0):
    m = canvas != where_bg
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]
    nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    nb[:, 0] |= m[:, -1]; nb[:, -1] |= m[:, 0]
    canvas[(~m) & nb] = color


# ------------------------------------------------------------------ FAR layer (BG3) 256 x 160
def far_layer():
    W, H = 256, 160
    c = np.zeros((H, W), np.uint8)
    # double-bubble moon: echo of the cover's headlight
    for (cx, cy, r) in ((176, 34, 21), (204, 44, 14)):
        ins, v, sp = sphere_shade(2 * r, 2 * r, (-0.4, -0.5))
        shade_obj(c, cx - r, cy - r, ins, v, sp, [8, 7, 6], spec_idx=6)
    # thin ring around the big one
    for t in np.linspace(0, 2 * math.pi, 400):
        x = 176 + math.cos(t) * 34; y = 34 + math.sin(t) * 7
        if not (math.sin(t) < 0 and abs(math.cos(t)) < 0.62):
            xi, yi = int(round(x)), int(round(y))
            if 0 <= yi < H:
                c[yi, xi % W] = 9 if math.sin(t) > 0 else 4
    # skyline: far bubble towers, flat-lit lavender
    base = 116
    x = 0
    towers = []
    while x < W:
        kind = rng.integers(0, 4)
        w = int(rng.integers(10, 22))
        h = int(rng.integers(18, 46))
        towers.append((x, w, h, kind))
        x += w + int(rng.integers(-3, 6))
    for (x, w, h, kind) in towers:
        if kind == 0:   # capsule tower
            ins, v, sp = capsule_shade(h, w, -0.7)
            shade_obj(c, x, base - h, ins, v * 0.7, sp, [1, 2, 3, 4], spec_idx=5, spec_th=0.75)
        elif kind == 1:  # stacked bubbles
            yy = base
            r = w // 2
            while yy - 2 * r > base - h - 10 and r >= 3:
                ins, v, sp = sphere_shade(2 * r, 2 * r)
                shade_obj(c, x + (w // 2 - r), yy - 2 * r, ins, v * 0.75, sp, [1, 2, 3, 4], spec_idx=5)
                yy -= 2 * r - 2
                r = max(3, r - 2)
        elif kind == 2:  # spire with a ball
            cx = x + w // 2
            for yy in range(base - h, base):
                ww = max(1, int((yy - (base - h)) / h * w / 2))
                for xx in range(cx - ww // 2, cx + ww // 2 + 1):
                    c[yy, xx % W] = 2 if xx < cx else 1
            ins, v, sp = sphere_shade(8, 8)
            shade_obj(c, cx - 4, base - h - 6, ins, v, sp, [2, 3, 4, 5], spec_idx=9)
        else:  # dome
            r = w // 2 + 3
            ins, v, sp = sphere_shade(2 * r, 2 * r)
            ins = ins.copy(); ins[r:, :] = False
            shade_obj(c, x + w // 2 - r, base - r, ins, v * 0.7, sp, [1, 2, 3, 4], spec_idx=9, spec_th=0.8)
    # distant window glints
    for _ in range(70):
        xx = int(rng.integers(0, W)); yy = int(rng.integers(80, base))
        if c[yy, xx] in (1, 2):
            c[yy, xx] = 11 if rng.random() < 0.6 else 9
    # haze band to the bottom
    c[base:, :] = 10
    for xx in range(W):
        if (xx % 4) < 2:
            c[base - 1, xx] = 10 if c[base - 1, xx] == 0 else c[base - 1, xx]
    return c


# ------------------------------------------------------------------ MID layer (BG2) 512 x 160
def mid_layer():
    W, H = 512, 160
    c = np.zeros((H, W), np.uint8)
    base = 128
    # glossy pink chrome buildings
    x = 4
    while x < W - 8:
        kind = int(rng.integers(0, 4))
        w = int(rng.integers(22, 40))
        h = int(rng.integers(34, 66))
        if kind == 0:   # capsule tower with oval windows
            ins, v, sp = capsule_shade(h, w, -0.6)
            shade_obj(c, x, base - h, ins, v, sp, [2, 3, 4, 5, 6], spec_idx=7, spec_th=0.5)
            for wy in range(base - h + w // 2 + 2, base - 6, 9):
                for wx in range(x + 5, x + w - 7, 8):
                    for dy in range(4):
                        for dx in range(3):
                            if not ((dy in (0, 3)) and dx in (0, 2)):
                                c[wy + dy, (wx + dx) % W] = 9 if dx == 0 else 8
        elif kind == 1:  # glass dome on a pink drum
            dh = 14
            for yy in range(base - dh, base):
                for xx in range(x, x + w):
                    t = (xx - x) / w
                    c[yy, xx % W] = [5, 4, 4, 3, 2][min(4, int(abs(t - 0.3) * 6))]
            r = w // 2
            ins, v, sp = sphere_shade(2 * r, 2 * r)
            ins = ins.copy(); ins[r:, :] = False
            shade_obj(c, x, base - dh - r, ins, v, sp, [11, 8, 9, 12], spec_idx=12, spec_th=0.4)
        elif kind == 2:  # ring tower: torus standing on a stem
            r = w // 2 + 4
            cx, cy = x + w // 2, base - h + r
            for yy in range(cy - r, cy + r):
                for xx in range(cx - r, cx + r):
                    d = math.hypot(xx + 0.5 - cx, yy + 0.5 - cy)
                    if r - 7 <= d <= r:
                        nrm = (d - (r - 3.5)) / 3.5
                        light = 0.55 - 0.45 * ((xx - cx) / r) - 0.35 * ((yy - cy) / r) - nrm * 0.3
                        c[yy, xx % W] = [2, 3, 4, 5, 6, 7][int(np.clip(light, 0, 0.99) * 6)]
            for yy in range(cy + r - 2, base):
                for xx in range(cx - 3, cx + 3):
                    c[yy, xx % W] = 4 if xx < cx else 3
        else:  # stacked pearl bubbles
            yy = base; r = w // 2
            while r >= 5 and yy - 2 * r > base - h - 12:
                ins, v, sp = sphere_shade(2 * r, 2 * r)
                shade_obj(c, x + w // 2 - r, yy - 2 * r, ins, v, sp, [3, 4, 5, 6, 7], spec_idx=12, spec_th=0.5)
                yy -= 2 * r - 3; r -= 3
        x += w + int(rng.integers(6, 20))
    # elevated curvy skyway in front of the buildings
    def sky_y(xx):
        return 88 + 6 * math.sin(xx / W * 2 * math.pi * 2) + 3 * math.sin(xx / W * 2 * math.pi * 3)
    for xx in range(W):
        y = int(round(sky_y(xx)))
        for k, col in enumerate([7, 6, 5, 13, 4, 3, 2, 1]):
            c[y + k, xx] = col
        # glow dots on the edge
        if xx % 16 == 0:
            c[y + 3, xx] = 9
    # supports every 64 px
    for sx in range(16, W, 64):
        y = int(round(sky_y(sx))) + 8
        for yy in range(y, base):
            for k in range(6):
                c[yy, sx + k] = [6, 5, 4, 4, 3, 2][k]
        c[y:y + 2, sx - 1:sx + 7] = 3
    # lavender undergrowth / lower blocks up to the road
    for xx in range(W):
        top = base - 6 + int(3 * math.sin(xx / 13.0) + 2 * math.sin(xx / 5.3))
        for yy in range(top, H):
            c[yy, xx] = 14 if (yy + xx) % 7 else 11
    outline_mask(c, 1)
    return c


# ------------------------------------------------------------------ ROAD (BG1) tiles
ROAD_TOP = 120     # first road pixel row (tile row 15)


def road_strip():
    """64 x 40 road pattern for rows 120..159"""
    W, H = 64, 40
    c = np.zeros((H, W), np.uint8)
    c[0, :] = 9; c[1, :] = 8; c[2, :] = 8; c[3, :] = 1
    c[4, :] = 7; c[5, :] = 15; c[6, :] = 6; c[7, :] = 10
    for y in range(8, 36):
        t = (y - 8) / 28
        v = np.full(W, 0.75 - t * 0.55)
        row = dither_ramp(v[None, :], [2, 3, 4], 0, y)[0]
        c[y] = row
    # lane dashes
    c[20:22, 4:28] = 5
    c[22, 4:28] = 12
    # glossy reflection streaks
    for y, x0, x1 in ((11, 36, 52), (27, 40, 50), (32, 8, 16)):
        c[y, x0:x1] = 4
    c[36, :] = 10; c[37, :] = 6; c[38, :] = 15; c[39, :] = 1
    return c


def pad_tiles():
    """hover pad: 2 tiles tall; returns 8x16 tiles: left cap, mid A, mid B, right cap"""
    W, H = 32, 16
    c = np.zeros((H, W), np.uint8)
    for x in range(W):
        for y in range(H):
            if y < 2:
                col = 13 if y == 0 else 12
            elif y < 6:
                col = 12 if y < 4 else 11
            elif y < 7:
                col = 1
            elif y < 9:
                col = 8 if (x % 8) in (2, 3, 4, 5) else 0
            elif y < 12:
                col = 9 if (x % 8) in (3, 4) and y < 11 else 0
            else:
                col = 0
            c[y, x] = col
    # rounded caps
    for y in range(7):
        for x in range(4 - min(4, y + 1) if y < 3 else 0):
            pass
    c[0, 0:3] = 0; c[1, 0:2] = 0; c[2, 0] = 0
    c[0, 29:32] = 0; c[1, 30:32] = 0; c[2, 31] = 0
    c[6, 0] = 0; c[6, 31] = 0
    # outline top edge
    out = c.copy()
    m = c > 0
    for y in range(H):
        for x in range(W):
            if not m[y, x] and ((y + 1 < H and m[y + 1, x]) or (x + 1 < W and m[y, x + 1] and y < 7) or (x > 0 and m[y, x - 1] and y < 7)):
                out[y, x] = 1
    return out


# ------------------------------------------------------------------ cars (side view, facing left)
CAR_PALS = {
    'bolt': ['#000000', '#140a20', '#1a5a8a', '#2a8ac0', '#48b8e8', '#9ae6ff', '#ffffff', '#ff5a8c', '#ffd24a', '#fff6c0', '#26203a', '#5a4a7a', '#a8f6ff', '#ff9ac8', '#3a2e56', '#ff3a6a'],
    'orca': ['#000000', '#140a20', '#9aa0bc', '#c8cce0', '#ecedf6', '#ffffff', '#ffffff', '#ff7a2c', '#ffd24a', '#fff6c0', '#26203a', '#5a4a7a', '#a8f6ff', '#ff9ac8', '#3a2e56', '#ffb070'],
    'wasp': ['#000000', '#140a20', '#4a8a10', '#7ac424', '#b4ec3c', '#e8ffa0', '#ffffff', '#202030', '#ffd24a', '#fff6c0', '#26203a', '#5a4a7a', '#a8f6ff', '#ff9ac8', '#3a2e56', '#3a3a50'],
    'manta': ['#000000', '#140a20', '#5a1a5a', '#9a2a88', '#d24ab4', '#ff9ae0', '#ffffff', '#20e0d0', '#ffd24a', '#fff6c0', '#26203a', '#5a4a7a', '#a8f6ff', '#ff9ac8', '#3a2e56', '#10a098'],
}
# car palette slots: 1 outline, 2-5 body ramp, 6 spec, 7 accent, 8-9 thruster, 10-11 canopy dark, 12 glass glint, 13 under-glow, 14 hull dark, 15 accent dark


def poly_mask(pts, w=64, h=32):
    im = Image.new('L', (w, h), 0)
    ImageDraw.Draw(im).polygon(pts, fill=1)
    return np.array(im, bool)


CAR_SHAPES = {
    # body polygon, canopy polygon, stripe y-range, thruster (x, y), hitbox (x0, y0, x1, y1) in sprite coords
    'bolt': dict(body=[(2, 22), (10, 16), (26, 13), (44, 12), (58, 13), (62, 17), (62, 24), (56, 27), (8, 27), (2, 25)],
                 canopy=[(22, 14), (28, 8), (40, 7), (46, 12)], stripe=(19, 20), thr=(62, 20), hit=(4, 10, 60, 28)),
    'orca': dict(body=[(4, 12), (10, 6), (52, 5), (60, 8), (62, 14), (62, 24), (58, 28), (8, 28), (3, 24)],
                 canopy=[(6, 12), (10, 7), (20, 7), (20, 14)], stripe=(18, 20), thr=(62, 18), hit=(4, 5, 61, 28)),
    'wasp': dict(body=[(14, 20), (20, 14), (36, 12), (48, 14), (52, 18), (52, 24), (46, 27), (18, 27), (13, 24)],
                 canopy=[(22, 14), (26, 9), (36, 9), (40, 13)], stripe=(21, 22), thr=(52, 20), hit=(14, 9, 52, 27)),
    'manta': dict(body=[(1, 24), (8, 19), (30, 16), (50, 14), (56, 6), (61, 6), (62, 16), (63, 24), (58, 27), (4, 27)],
                  canopy=[(20, 17), (26, 12), (38, 11), (44, 15)], stripe=(21, 22), thr=(63, 21), hit=(3, 8, 62, 28)),
}


def draw_car(name, frame):
    s = CAR_SHAPES[name]
    c = np.zeros((32, 64), np.uint8)
    body = poly_mask(s['body'])
    ys, xs = np.nonzero(body)
    top, bot = ys.min(), ys.max()
    yy, xx = np.mgrid[0:32, 0:64]
    v = 1 - (yy - top) / max(1, bot - top)
    v = v * 0.9 + 0.08 + 0.08 * np.sin(xx / 6.0) * 0
    idx = dither_ramp(v, [14, 2, 3, 4, 5], 0, 0)
    c[body] = idx[body]
    # specular streak along the top edge
    for x in range(64):
        col = np.nonzero(body[:, x])[0]
        if len(col):
            t = col.min()
            if 6 < x < 58:
                c[t + 1, x] = 6 if (x // 3) % 5 else 5
    # accent stripe
    y0, y1 = s['stripe']
    for y in range(y0, y1 + 1):
        for x in range(64):
            if body[y, x]:
                c[y, x] = 7 if y == y0 else 15
    # canopy glass
    can = poly_mask(s['canopy'])
    cv = 1 - (yy - 6) / 10
    cidx = dither_ramp(cv, [10, 11, 14], 0, 0)
    c[can] = cidx[can]
    cys, cxs = np.nonzero(can)
    for k in range(3):
        gx = cxs.min() + 3 + k; gy = cys.min() + 1 + k // 2
        if can[gy, gx]:
            c[gy, gx] = 12
    # hull dark underside line
    for x in range(64):
        col = np.nonzero(body[:, x])[0]
        if len(col):
            c[col.max(), x] = 14
    # thruster at the back (right side): flame flickers
    tx, ty = s['thr']
    L = 4 if frame == 0 else 7
    for k in range(L):
        x = tx - 1 + k
        if x < 64:
            for dy in (-1, 0, 1):
                if abs(dy) <= (1 if k < L - 2 else 0):
                    c[ty + dy, x] = 9 if dy == 0 and k < L - 1 else 8
    # under-glow
    for x in range(64):
        col = np.nonzero(body[:, x])[0]
        if len(col) and col.max() + 2 < 32 and (x + frame * 2) % 6 < 4:
            c[col.max() + 2, x] = 13
    # outline
    m = c > 0
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]; nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    o = (~m) & nb
    # glow pixels don't get an outline below
    c[o] = 1
    return c


# ------------------------------------------------------------------ small sprites
ORB_PAL = ['#000000', '#1c0a2c', '#2a7ad0', '#4ac8f0', '#a8f4ff', '#ffffff', '#ff7ab4', '#ffc4e0', '#fff4a0', '#ffd24a',
           '#c03ca0', '#e8d8f8', '#8a6ab0', '#ff4a6a', '#ffffff', '#58e4ff']


def orb(frame):
    c = np.zeros((16, 16), np.uint8)
    ins, v, sp = sphere_shade(12, 12, (-0.5, -0.55))
    # iridescent: hue sweeps with the normal direction
    yy, xx = np.mgrid[0:12, 0:12]
    ang = np.arctan2(yy - 6, xx - 6) + frame * math.pi / 2
    hue = (np.sin(ang) * 0.5 + 0.5)
    idx = np.where(hue > 0.5, dither_ramp(v, [10, 6, 7, 5], 0, 0), dither_ramp(v, [2, 3, 4, 5], 0, 0))
    idx = np.where(sp > 0.5, 5, idx)
    for y in range(12):
        for x in range(12):
            if ins[y, x]:
                c[y + 2, x + 2] = idx[y, x]
    # outline
    m = c > 0
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]; nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    c[(~m) & nb] = 1
    # sparkle
    sx, sy = [(3, 3), (12, 4), (12, 12), (4, 12)][frame]
    for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
        if 0 <= sx + dx < 16 and 0 <= sy + dy < 16:
            c[sy + dy, sx + dx] = 14 if (dx, dy) == (0, 0) else 8
    return c


def dust(frame):
    c = np.zeros((8, 8), np.uint8)
    r = [1.5, 2.5, 3.2][frame]
    for y in range(8):
        for x in range(8):
            d = math.hypot(x - 3.5, y - 4.5)
            if d < r:
                c[y, x] = 11 if d < r - 1 else 12
    if frame == 2:
        c[::2, ::2] = 0
    return c


def sparkle(frame):
    c = np.zeros((8, 8), np.uint8)
    L = [3, 2][frame]
    for k in range(-L, L + 1):
        c[4, 4 + k] = 8 if abs(k) < L else 9
        c[4 + k, 4] = 8 if abs(k) < L else 9
    c[4, 4] = 14
    return c


def warn():
    c = np.zeros((16, 16), np.uint8)
    for y in range(14):
        for x in range(16):
            # triangle
            if y >= 1 and abs(x - 7.5) <= y * 0.55 + 0.5:
                c[y, x] = 9 if y < 12 else 9
    for y in range(1, 14):
        for x in range(16):
            if c[y, x] and (y == 13 or abs(x - 7.5) > y * 0.55 - 0.6):
                c[y, x] = 1
    c[4:9, 7:9] = 1
    c[10:12, 7:9] = 1
    return c


# ------------------------------------------------------------------ hero car (side view) and its ramp
HERO_PAL = ['#000000', '#1c0a2c', '#c0407c', '#ff76aa', '#ffa8c8', '#ffd8e8', '#ffffff', '#58e4ff', '#ffd24a', '#fff6c0',
            '#3a2e56', '#6a5a8a', '#c8faff', '#ff9ac8', '#7a1e52', '#a8f0ff']


def hero_side(frame):
    """two-seat open hover roadster, facing right (it drives off to the right)"""
    c = np.zeros((32, 64), np.uint8)
    body = poly_mask([(1, 18), (6, 12), (18, 11), (24, 15), (40, 15), (46, 11), (56, 12), (63, 17), (63, 22), (58, 27), (6, 27), (1, 23)])
    yy, xx = np.mgrid[0:32, 0:64]
    v = 1 - (yy - 11) / 16
    idx = dither_ramp(v * 0.95 + 0.05, [14, 2, 3, 4, 5], 0, 0)
    c[body] = idx[body]
    for x in range(64):
        col = np.nonzero(body[:, x])[0]
        if len(col):
            if 4 < x < 60:
                c[col.min() + 1, x] = 6 if (x // 4) % 4 else 5
            c[col.max(), x] = 14
    # chrome cyan stripe
    for x in range(64):
        if body[20, x]:
            c[20, x] = 7
            c[21, x] = 15
    # two cockpits (seats) cut into the top
    for cx in (21, 33):
        for x in range(cx - 4, cx + 5):
            c[13:15, x] = 10
    # headlight bubble at the front (right)
    for (hx, hy) in ((59, 17), (60, 17), (58, 18), (59, 18), (60, 18)):
        c[hy, hx] = 9
    c[17, 61] = 6
    # rear thruster (left)
    L = 4 if frame == 0 else 7
    for k in range(L):
        x = 1 - k
        if x >= 0:
            c[19:22, x] = 8
    for x in range(64):
        col = np.nonzero(body[:, x])[0]
        if len(col) and col.max() + 2 < 32 and (x + frame * 2) % 6 < 4:
            c[col.max() + 2, x] = 7
    m = c > 0
    nb = np.zeros_like(m)
    nb[1:, :] |= m[:-1, :]; nb[:-1, :] |= m[1:, :]; nb[:, 1:] |= m[:, :-1]; nb[:, :-1] |= m[:, 1:]
    c[(~m) & nb] = 1
    return c


def banner():
    """finish arch top: 'TURBO' neon sign 64x16"""
    from font5x7 import GLYPHS
    c = np.zeros((16, 64), np.uint8)
    c[1:15, 1:63] = 10
    c[0, 2:62] = 1; c[15, 2:62] = 1; c[1:15, 0] = 1; c[1:15, 63] = 1
    c[1, 1:63] = 11
    x = 6
    for ch in 'TURBO':
        g = GLYPHS[ch]
        for gy, row in enumerate(g):
            for gx, p in enumerate(row):
                if p == '#':
                    for sy in range(2):
                        for sx in range(2):
                            px, py = x + gx * 2 + sx, 1 + gy * 2 // 1 + sy
                            if py < 15:
                                c[py, px] = 3 if sy == 0 else 2
        x += 11
    return c


def main():
    out = CWriter(os.path.join(ROOT, 'src/gen/run_gen.c'), os.path.join(ROOT, 'src/gen/run_gen.h'), 'tools/gen_run.py')
    # ---- palettes
    out.u16('run_bg_pal', pal15(PAL_FAR) + pal15(PAL_MID) + pal15(PAL_ROAD))
    out.u16('hud_pal', pal15(PAL_HUD))
    out.u16('run_sky', sky_table(SKY_KEYS, 160))
    # ---- far
    far = far_layer()
    t, e = tiles_dedupe(far, 4, flip=True)
    out.u32('far_tiles', sum(t, []))
    out.u16('far_map', e)     # 32 x 20
    print('far tiles', len(t))
    # ---- mid
    mid = mid_layer()
    t2, e2 = tiles_dedupe(mid, 4, flip=True)
    out.u32('mid_tiles', sum(t2, []))
    # arrange 64x20 into two 32-wide screenblocks
    m2 = np.array(e2).reshape(20, 64)
    out.u16('mid_map', list(m2[:, :32].reshape(-1)) + list(m2[:, 32:].reshape(-1)))
    print('mid tiles', len(t2))
    # ---- road tiles (pattern 8x5) + pad tiles (4x2)
    road = road_strip()
    pad = pad_tiles()
    rt, re_ = tiles_dedupe(road, 4, flip=False)
    road_tiles = list(rt)
    pad_ids = []
    for ty in range(2):
        for tx in range(4):
            road_tiles.append(tile4(pad[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]))
            pad_ids.append(len(road_tiles) - 1)
    out.u32('road_tiles', sum(road_tiles, []))
    out.u16('road_pattern', re_)     # 8 x 5
    out.u16('pad_ids', pad_ids)      # top row L,M,M,R then bottom row
    print('road tiles', len(road_tiles))
    # ---- sprites
    obj_pal = []
    import chars as chars2
    from chars import palette as char_palette, ORDER as CHAR_ORDER
    for who in ('franco', 'tripleset'):
        obj_pal += pal15(char_palette(who))
    for n in ('bolt', 'orca', 'wasp', 'manta'):
        obj_pal += pal15(CAR_PALS[n])
    obj_pal += pal15(ORB_PAL)          # 6
    obj_pal += pal15(HERO_PAL)         # 7
    obj_pal += pal15(PAL_HUD)          # 8
    out.u16('run_obj_pal', obj_pal)
    # characters: all frames, streamed into VRAM per frame
    for who in ('franco', 'tripleset'):
        words = []
        for n in CHAR_ORDER:
            words += obj_tiles(chars2.compose(who, n), 4, 6)
        out.u32('char_%s' % who, words)
    out.define('CHAR_FRAMES', len(CHAR_ORDER))
    for i, n in enumerate(CHAR_ORDER):
        out.define('CF_%s' % n.upper(), i)
    # cars
    words = []
    hits = []
    for n in ('bolt', 'orca', 'wasp', 'manta'):
        for f in range(2):
            words += obj_tiles(draw_car(n, f), 8, 4)
        hits += list(CAR_SHAPES[n]['hit'])
    out.u32('car_tiles', words)
    out.u8('car_hit', hits)
    # hero car side 2 frames
    words = []
    for f in range(2):
        words += obj_tiles(hero_side(f), 8, 4)
    out.u32('hero_side_tiles', words)
    # orb (4), dust (3), sparkle (2), warn (1), banner
    words = []
    for f in range(4):
        words += obj_tiles(orb(f), 2, 2)
    out.u32('orb_tiles', words)
    words = []
    for f in range(3):
        words += obj_tiles(dust(f), 1, 1)
    for f in range(2):
        words += obj_tiles(sparkle(f), 1, 1)
    out.u32('fx_tiles', words)
    out.u32('warn_tiles', obj_tiles(warn(), 2, 2))
    out.u32('banner_tiles', obj_tiles(banner(), 8, 2))
    # font: white glyph with a dark drop shadow (palette: 1 dark, 2 white)
    from font5x7 import GLYPHS
    words = []
    for code in range(32, 128):
        ch = chr(code)
        g = GLYPHS.get(ch) or GLYPHS.get(ch.upper()) or GLYPHS.get('?')
        t = np.zeros((8, 8), np.uint8)
        for gy, row in enumerate(g):
            for gx, px in enumerate(row):
                if px == '#':
                    if gy + 1 < 8 and gx + 2 < 8:
                        t[gy + 1, gx + 2] = 1
                    t[gy, gx + 1] = 2
        words += tile4(t)
    out.u32('font_tiles', words)
    out.close()

    # ---- previews
    def to_rgb(img, pal, sky=None):
        rgb = np.zeros(img.shape + (3,), np.uint8)
        P = [hexc(p) for p in pal]
        for i in range(1, 16):
            rgb[img == i] = P[i]
        if sky is not None:
            for y in range(img.shape[0]):
                rgb[y][img[y] == 0] = sky[y]
        return rgb
    skyc = [tuple(int(v) for v in c) for c in grad(SKY_KEYS, 160)]
    scene = to_rgb(far[:, :240], PAL_FAR, skyc)
    midrgb = to_rgb(mid[:, 40:280], PAL_MID)
    m = mid[:, 40:280] > 0
    scene[m] = midrgb[m]
    rd = np.zeros((160, 240), np.uint8)
    for x in range(240):
        rd[120:160, x] = road[:, x % 64]
    rr = to_rgb(rd, PAL_ROAD)
    m = rd > 0
    scene[m] = rr[m]
    Image.fromarray(scene).resize((480, 320), Image.NEAREST).save(os.path.join(PREV, 'run_bg.png'))
    sheet = np.zeros((32, 64 * 4, 3), np.uint8); sheet[:] = (240, 200, 220)
    for i, n in enumerate(('bolt', 'orca', 'wasp', 'manta')):
        cimg = draw_car(n, 1)
        r = to_rgb(cimg, CAR_PALS[n]); mm = cimg > 0
        sheet[:, i * 64:(i + 1) * 64][mm] = r[mm]
    Image.fromarray(sheet).resize((64 * 4 * 3, 96), Image.NEAREST).save(os.path.join(PREV, 'cars.png'))


if __name__ == '__main__':
    main()
