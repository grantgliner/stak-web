#!/usr/bin/env python3
"""Generate STAK! GBA graphics: palettes, font, UI panels, base slab, the 8bpp
title logo (converted from ../images/stak-logo.png) and a theme per mode: block
colours, panel colours, sky gradient and a background layer (relaxed: clouds
over a sunset beach; classic: stars; expert: champagne bubbles and gold
sparkles on velvet)."""
import os, sys, random, math
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from font5x7 import GLYPHS

OUT_C = os.path.join(ROOT, 'src/gen/gfx_gen.c')
OUT_H = os.path.join(ROOT, 'src/gen/gfx_gen.h')
PREV = os.path.join(ROOT, 'build/preview')
os.makedirs(PREV, exist_ok=True)

def rgb15(c):
    r, g, b = [max(0, min(31, int(round(v / 255 * 31)))) for v in c]
    return r | (g << 5) | (b << 10)

def hexc(h):
    h = h.lstrip('#'); return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

def tiles4(img):
    """img: 2D array of palette indices (h, w multiples of 8) -> list of u32 words, row-major tiles"""
    h, w = img.shape; words = []
    for ty in range(h // 8):
        for tx in range(w // 8):
            for row in img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]:
                v = 0
                for k in range(8): v |= int(row[k]) << (4 * k)
                words.append(v)
    return words

def tiles4_obj(img, tw, th):
    """sprite 1D tile order for a tw x th tile sprite"""
    return tiles4(img[:th * 8, :tw * 8])

# ---------------------------------------------------------------- palettes
CLASSIC = ['#FF8A00', '#FFC700', '#00FF27', '#00D1FF', '#0075FF', '#AD00FF', '#FF00E5']

def ramp(base, gloss=True):
    c = np.array(hexc(base) if isinstance(base, str) else base, float)
    pal = [(0, 0, 0)] * 16
    pal[1] = tuple(c * 0.22 + np.array([8, 8, 18]))           # outline
    for k in range(8):
        t = k / 7
        col = c * (0.42 + 0.70 * t)
        if t > 0.7:
            col = col + (255 - col) * (t - 0.7) * 1.1
        pal[2 + k] = tuple(np.clip(col, 0, 255))
    pal[10] = tuple(np.clip(c * 0.25 + 200, 0, 255))           # specular
    return [rgb15(p) for p in pal]

MODES = ['relaxed', 'classic', 'expert']
BLOCK_COLORS = {                    # the web version's tints per mode
    'relaxed': ['#62FFFD', '#5BFF89', '#FFB74A', '#FFEE58'],
    'classic': CLASSIC,
    'expert': ['#FFFFFF', '#555555', '#CEA63D'],
}
def block_banks(cols):
    banks = [ramp(c) for c in cols]
    while len(banks) < 7: banks.append([0] * 16)
    return [v for b in banks for v in b]
mode_block_pal = [block_banks(BLOCK_COLORS[m]) for m in MODES]

obj_pal = []
for c in CLASSIC: obj_pal.append(ramp(c))
obj_pal.append(ramp('#FF2A2A'))     # 7: collision red
obj_pal.append(ramp('#9A9AA2'))     # 8: base (UIColor.gray, metallic)
ui = [0] * 16
ui[1] = rgb15((255, 255, 255)); ui[2] = rgb15((20, 22, 40)); ui[3] = rgb15((255, 214, 64))
ui[4] = rgb15((255, 138, 0)); ui[5] = rgb15((120, 220, 255))
obj_pal.append(ui)                  # 9: UI sprites
while len(obj_pal) < 16: obj_pal.append([0] * 16)

# BG palettes 0-2 text, 3 panel, 4 panel highlighted (cursor), 5 background layer
def textpal(fg, panel_fill, sh=(16, 18, 36)):
    p = [0] * 16; p[1] = rgb15(fg); p[2] = rgb15(sh)
    p[4] = rgb15(panel_fill)          # behind text on a panel
    return p
def panelpal(border, fill, inner):
    p = [0] * 16; p[1] = rgb15(border); p[2] = rgb15(fill); p[3] = rgb15(inner); p[4] = rgb15((10, 12, 26))
    return p
THEMES = {
    # panel (border, fill, inner), highlighted panel, text grey, layer colours
    'relaxed': dict(panel=((255, 226, 168), (20, 86, 108), (28, 102, 124)), hi=((255, 120, 84), (78, 48, 52), (98, 58, 58)),
                    grey=(200, 226, 232),
                    layer=[(255, 255, 255), (255, 244, 236), (255, 218, 206), (236, 190, 206), (190, 160, 200), (70, 62, 96)]),
    'classic': dict(panel=((70, 82, 140), (20, 25, 52), (28, 34, 68)), hi=((255, 170, 30), (52, 40, 40), (70, 52, 44)),
                    grey=(150, 160, 200),
                    layer=[(255, 255, 255), (200, 210, 255), (140, 150, 210), (90, 100, 160), (255, 230, 170), (170, 255, 240)]),
    'expert': dict(panel=((206, 166, 61), (16, 12, 20), (26, 20, 30)), hi=((250, 246, 236), (44, 34, 26), (60, 46, 34)),
                   grey=(196, 178, 150),
                   layer=[(224, 210, 180), (176, 146, 92), (122, 94, 50), (96, 72, 40), (240, 232, 214), (120, 82, 120)]),
}
def bg_palette(t):
    fill = t['panel'][1]
    banks = [textpal((255, 255, 255), fill), textpal((255, 210, 60), fill), textpal(t['grey'], fill),
             panelpal(*t['panel']), panelpal(*t['hi'])]
    layer = [0] * 16
    for i, c in enumerate(t['layer']): layer[1 + i] = rgb15(c)
    banks.append(layer)
    return [v for b in banks for v in b]
mode_bg_pal = [bg_palette(THEMES[m]) for m in MODES]
bg_pal = [mode_bg_pal[1][i * 16:(i + 1) * 16] for i in range(6)]

# ---------------------------------------------------------------- font (BG + OBJ use same layout)
font = []; font_panel = []
for code in range(32, 128):
    ch = chr(code)
    g = GLYPHS.get(ch.upper(), GLYPHS['?'] if ch not in GLYPHS else GLYPHS[ch])
    img = np.zeros((8, 8), int)
    for y, row in enumerate(g):
        for x, c in enumerate(row):
            if c == '#':
                if img[y + 1, x + 2] == 0: img[y + 1, x + 2] = 2 if y + 1 < 8 else 0
    for y, row in enumerate(g):
        for x, c in enumerate(row):
            if c == '#': img[y, x + 1] = 1
    font.extend(tiles4(img))
    pimg = img.copy(); pimg[pimg == 0] = 4
    font_panel.extend(tiles4(pimg))

# big digits (2x scale, 16x16 = 2x2 tiles each) for the score
big = []
for d in '0123456789':
    g = GLYPHS[d]
    img = np.zeros((16, 16), int)
    for y, row in enumerate(g):
        for x, c in enumerate(row):
            if c == '#':
                for dy in range(2):
                    for dx in range(2):
                        yy, xx = 1 + y * 2 + dy + 1, 2 + x * 2 + dx + 1
                        if yy < 16 and xx < 16 and img[yy, xx] == 0: img[yy, xx] = 2
    for y, row in enumerate(g):
        for x, c in enumerate(row):
            if c == '#':
                for dy in range(2):
                    for dx in range(2): img[1 + y * 2 + dy, 2 + x * 2 + dx] = 1
    # tiles in order TL, TR, BL, BR
    for ty in range(2):
        for tx in range(2):
            big.extend(tiles4(img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]))

# ---------------------------------------------------------------- panel tiles (9-slice, rounded)
def panel_tiles():
    T = {}
    def blank(): return np.zeros((8, 8), int)
    # fill
    f = blank() + 2; T['fill'] = f
    # top edge
    t = blank() + 2; t[0, :] = 4; t[1, :] = 1; T['top'] = t
    b = blank() + 2; b[7, :] = 4; b[6, :] = 1; T['bot'] = b
    l = blank() + 2; l[:, 0] = 4; l[:, 1] = 1; T['left'] = l
    r = blank() + 2; r[:, 7] = 4; r[:, 6] = 1; T['right'] = r
    def corner(flipx, flipy):
        c = blank() + 2
        for y in range(8):
            for x in range(8):
                dx, dy = 7.5 - x, 7.5 - y   # distance from inner corner
                d = math.hypot(max(0, 5.5 - x), max(0, 5.5 - y))
                if d > 5.6: c[y, x] = 0
                elif d > 4.6: c[y, x] = 4
                elif d > 3.6: c[y, x] = 1
        if flipx: c = c[:, ::-1]
        if flipy: c = c[::-1, :]
        return c
    T['tl'] = corner(0, 0); T['tr'] = corner(1, 0); T['bl'] = corner(0, 1); T['br'] = corner(1, 1)
    order = ['fill', 'top', 'bot', 'left', 'right', 'tl', 'tr', 'bl', 'br']
    words = []
    for k in order: words.extend(tiles4(T[k]))
    return words
panel = panel_tiles()

# ---------------------------------------------------------------- starfield (32x32 map, 16 unique tiles)
random.seed(7)
star_tiles = [np.zeros((8, 8), int)]
for i in range(15):
    t = np.zeros((8, 8), int)
    n = 1 if i < 10 else 2
    for _ in range(n):
        x, y = random.randrange(1, 7), random.randrange(1, 7)
        c = random.choice([1, 2, 3, 3, 4, 4, 5, 6])
        t[y, x] = c
        if i >= 13:  # sparkle
            t[y - 1, x] = t[y + 1, x] = t[y, x - 1] = t[y, x + 1] = 3
    star_tiles.append(t)
star_map = [random.choice([0] * 6 + list(range(1, 16))) for _ in range(32 * 32)]

def tiled(img):
    """256x256 index image -> (unique tiles, 32x32 map), tile 0 blank"""
    tiles, index, tmap = [np.zeros((8, 8), int)], {bytes(8 * 8): 0}, []
    for ty in range(32):
        for tx in range(32):
            t = img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
            k = t.astype(np.uint8).tobytes()
            if k not in index:
                index[k] = len(tiles); tiles.append(t.copy())
            tmap.append(index[k])
    return tiles, tmap

# relaxed: puffy clouds lit from the low sun (layer colours 1 white .. 4 lilac shadow, 6 gull)
def clouds():
    rnd = random.Random(11)
    img = np.zeros((256, 256), int)
    spots = [(20, 26, 70), (150, 14, 56), (96, 80, 84), (8, 134, 52), (176, 118, 64), (60, 190, 76), (190, 210, 46)]
    for x0, y0, w in spots:
        h = w // 3
        yy, xx = np.mgrid[0:h + 12, -8:w + 8]
        x0 -= 8
        m = np.zeros(yy.shape, bool)
        base = h + 6                                   # flat bottom
        m |= (((xx - w / 2) / (w / 2)) ** 2 + ((yy - base) / (h * 0.55)) ** 2 <= 1) & (yy <= base)
        for _ in range(4):                             # puffs on top
            cx = rnd.uniform(w * 0.22, w * 0.78); r = rnd.uniform(h * 0.45, h * 0.8)
            cy = base - h * 0.35 - rnd.uniform(0, h * 0.3)
            m |= ((xx - cx) ** 2 + (yy - cy) ** 2 <= r * r) & (yy <= base)
        # light from the upper left: white top, warm and lilac shadows underneath
        lvl = np.full(yy.shape, 1)
        sh = (yy - (base - h * 0.9)) / (h * 0.9) + (xx / w - 0.5) * 0.35
        assert not (m[:, 0] | m[:, -1]).any()
        lvl[sh > 0.55] = 2; lvl[sh > 0.8] = 3; lvl[sh > 1.0] = 4
        sub = img[y0:y0 + m.shape[0], x0:x0 + m.shape[1]]
        mm = m[:sub.shape[0], :sub.shape[1]]
        sub[mm] = lvl[:sub.shape[0], :sub.shape[1]][mm]
    for gx, gy in [(122, 52), (134, 58), (40, 160), (214, 182)]:   # gulls
        for dx, dy in [(-3, -1), (-2, 0), (-1, 0), (0, 1), (1, 0), (2, 0), (3, -1)]:
            img[gy + dy, gx + dx] = 6
    return img

# expert: rising champagne bubbles and gold sparkles
def bubbles():
    rnd = random.Random(5)
    img = np.zeros((256, 256), int)
    def ring(cx, cy, r):
        for y in range(-r - 1, r + 2):
            for x in range(-r - 1, r + 2):
                d = math.hypot(x, y)
                if abs(d - r) < 0.6: img[(cy + y) % 256, (cx + x) % 256] = 2 if (x + y) < 0 else 3
        img[(cy - r // 2) % 256, (cx - r // 2) % 256] = 1      # glint
    for _ in range(7):                                          # columns of bubbles
        x = rnd.randrange(8, 248); y = rnd.randrange(256)
        for k in range(rnd.randrange(3, 7)):
            ring(x + rnd.randrange(-2, 3), y, rnd.choice([1, 1, 2, 2, 3]))
            y = (y + rnd.randrange(14, 30)) % 256
    for _ in range(18):                                         # sparkles
        x, y = rnd.randrange(4, 252), rnd.randrange(4, 252)
        if rnd.random() < 0.4:
            img[y, x] = 5
            for d in (1, 2): img[y - d, x] = img[y + d, x] = img[y, x - d] = img[y, x + d] = 2 if d == 1 else 4
        else:
            img[y, x] = rnd.choice([2, 4])
    return img

layer_tiles, layer_maps = [], []
for m in MODES:
    if m == 'classic':
        t, mp = star_tiles, star_map
    else:
        t, mp = tiled(clouds() if m == 'relaxed' else bubbles())
    words = []
    for tile in t: words.extend(tiles4(tile))
    layer_tiles.append(words); layer_maps.append(mp)
    prev = np.array([[0, 0, 0]] + [list(c) for c in THEMES[m]['layer']] + [[0, 0, 0]] * 9, np.uint8)
    full = np.zeros((256, 256), int)
    for i, v in enumerate(mp): full[(i // 32) * 8:(i // 32) * 8 + 8, (i % 32) * 8:(i % 32) * 8 + 8] = t[v]
    Image.fromarray(prev[full]).save(os.path.join(PREV, f'layer_{m}.png'))
    print(f'layer {m}: {len(t)} tiles')

# ---------------------------------------------------------------- base slab sprite (98x24 -> 128x32, 2 x 64x32)
def base_slab():
    W, H = 128, 32
    SS = 4
    bw, bh = 98, 24
    m = np.zeros((H * SS, W * SS), bool)
    x0 = (W - bw) // 2 * SS; y0 = 0
    m[y0:y0 + bh * SS, x0:x0 + bw * SS] = True
    # round corners slightly
    r = 3 * SS
    for (cy, cx) in [(y0 + r, x0 + r), (y0 + r, x0 + bw * SS - r), (y0 + bh * SS - r, x0 + r), (y0 + bh * SS - r, x0 + bw * SS - r)]:
        yy, xx = np.mgrid[0:H * SS, 0:W * SS]
        corner = ((yy < y0 + r) | (yy >= y0 + bh * SS - r)) & ((xx < x0 + r) | (xx >= x0 + bw * SS - r))
        m &= ~(corner & (np.hypot(yy - cy, xx - cx) > r) & (abs(yy - cy) < r + 1) & (abs(xx - cx) < r + 1) & (((yy < y0 + r) == (cy < y0 + bh * SS / 2)) & ((xx < x0 + r) == (cx < x0 + bw * SS / 2))))
    mm = m.reshape(H, SS, W, SS).mean(axis=(1, 3)) >= 0.5
    img = np.zeros((H, W), int)
    ys, xs = np.nonzero(mm)
    for y, x in zip(ys, xs):
        ty = (y - 0) / bh
        lvl = 5.4 - ty * 3.2 - 0.5 * abs(x - 63.5) / 49     # symmetric: the right half is the left one flipped
        # top surface highlight (the base is seen slightly from above in the original)
        if y <= 2: lvl = 7.5
        elif y <= 4: lvl = 6.2
        img[y, x] = 2 + int(max(0, min(7, lvl)))
    # outline
    pad = np.pad(mm, 1)
    edge = mm & ~(pad[:-2, 1:-1] & pad[2:, 1:-1] & pad[1:-1, :-2] & pad[1:-1, 2:])
    img[edge] = 1
    img[1, :][mm[1, :] & ~edge[1, :]] = 10
    return img
slab = base_slab()
slab[:, 64:] = slab[:, 63::-1]                          # exactly symmetric
base_words = tiles4_obj(slab[:, 0:64], 8, 4)           # left half; drawn twice, flipped

# ---------------------------------------------------------------- sky gradient (BG backdrop via HBlank DMA)
# 1024 lines: index 0 = highest point (deep space) ... 1023 = horizon at the base
SKY_STOPS = {
    # relaxed: a beach at sunset, warm at the base, blue dusk higher up
    'relaxed': [(0, (14, 18, 56)), (300, (28, 44, 108)), (560, (50, 88, 168)), (760, (92, 128, 200)),
                (870, (156, 140, 200)), (950, (226, 150, 160)), (1000, (246, 172, 140)), (1023, (252, 196, 146))],
    'classic': [(0, (2, 2, 8)), (380, (6, 8, 28)), (640, (12, 18, 52)), (840, (24, 30, 82)), (960, (64, 46, 104)), (1023, (120, 70, 110))],
    # expert: black velvet with a wine-red glow at the base
    'expert': [(0, (4, 2, 6)), (400, (14, 6, 20)), (700, (30, 10, 36)), (880, (50, 16, 50)), (980, (76, 26, 60)), (1023, (100, 40, 70))],
}
def gradient(stops):
    out = []
    for i in range(1024):
        for k in range(len(stops) - 1):
            a, ca = stops[k]; b, cb = stops[k + 1]
            if a <= i <= b:
                t = (i - a) / (b - a)
                out.append(rgb15([ca[j] + (cb[j] - ca[j]) * t for j in range(3)])); break
    return out
skies = [gradient(SKY_STOPS[m]) for m in MODES]

# ---------------------------------------------------------------- title logo (8bpp)
LOGO_W, LOGO_H = 208, 88
def logo():
    im = Image.open(os.path.join(ROOT, '..', 'images', 'stak-logo.png')).convert('RGBA')
    h = round(im.height * LOGO_W / im.width)
    s = im.resize((LOGO_W, h), Image.LANCZOS)
    a = np.array(s)[..., 3]
    rgb = np.array(s)[..., :3].astype(float)
    # un-premultiply-ish: flatten semi-transparent edges onto the dark outline colour
    solid = a >= 110
    # 1px navy outline outside everything (crisp pixel-art edge)
    pad = np.pad(solid, 1)
    grow = solid | pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]
    outline = grow & ~solid
    rgbs = rgb.copy()
    rgbs[outline] = (8, 10, 24)
    mask = grow
    # quantize solid colours to <= 150 colours
    flat = Image.fromarray(np.where(mask[..., None], rgbs, 0).astype(np.uint8))
    q = flat.quantize(colors=150, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.array(q)
    pal = q.getpalette()[:150 * 3]
    out = np.zeros((LOGO_H, LOGO_W), int)
    hh = min(h, LOGO_H)
    oy = (LOGO_H - hh) // 2
    sub = np.where(mask[:hh], idx[:hh] + 96, 0)
    out[oy:oy + hh] = sub
    colors = [rgb15(pal[i * 3:i * 3 + 3]) for i in range(150)]
    prev = np.zeros((LOGO_H, LOGO_W, 3), np.uint8) + np.array([11, 16, 34], np.uint8)
    for i in range(150):
        prev[out == 96 + i] = pal[i * 3:i * 3 + 3]
    Image.fromarray(prev).resize((LOGO_W * 3, LOGO_H * 3), Image.NEAREST).save(os.path.join(PREV, 'logo.png'))
    # 8bpp tiles
    words = []
    for ty in range(LOGO_H // 8):
        for tx in range(LOGO_W // 8):
            for row in out[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]:
                for k in range(0, 8, 4):
                    words.append(int(row[k]) | int(row[k + 1]) << 8 | int(row[k + 2]) << 16 | int(row[k + 3]) << 24)
    return words, colors
logo_words, logo_colors = logo()

# ---------------------------------------------------------------- UI sprites: cursor arrow (16x16), "+" score tag font reuse
def arrow():
    img = np.zeros((16, 16), int)
    pts = ["##      ", "####    ", "######  ", "########", "######  ", "####    ", "##      "]
    for y, row in enumerate(pts):
        for x, c in enumerate(row):
            if c == '#':
                img[y + 4, x + 4] = 4
    # outline
    m = img > 0
    pad = np.pad(m, 1)
    o = (pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]) & ~m
    img[o] = 2
    return tiles4_obj(img[0:16, 0:16], 2, 2)[:0] or (tiles4(img[0:8, 0:8]) + tiles4(img[0:8, 8:16]) + tiles4(img[8:16, 0:8]) + tiles4(img[8:16, 8:16]))

# ---------------------------------------------------------------- write
def arr(name, ctype, data, per=12, fmt='0x{:08X}'):
    s = f'const {ctype} {name}[{len(data)}] __attribute__((aligned(4))) = {{\n'
    for i in range(0, len(data), per):
        s += '  ' + ','.join(fmt.format(v) for v in data[i:i + per]) + ',\n'
    return s + '};\n'

with open(OUT_H, 'w') as f:
    f.write('/* generated by tools/gen_gfx.py */\n#pragma once\n#include "../gba.h"\n')
    f.write(f'#define LOGO_W {LOGO_W}\n#define LOGO_H {LOGO_H}\n#define LOGO_COLOR_BASE 96\n#define LOGO_COLORS {len(logo_colors)}\n')
    for n, t, d in [('obj_pal', 'u16', 256), ('font_tiles', 'u32', len(font)), ('font_panel_tiles', 'u32', len(font_panel)), ('bigdigit_tiles', 'u32', len(big)),
                    ('panel_tiles', 'u32', len(panel)), ('base_tiles', 'u32', len(base_words)), ('logo_tiles', 'u32', len(logo_words)),
                    ('logo_pal', 'u16', len(logo_colors))]:
        f.write(f'extern const {t} {n}[{d}];\n')
    f.write('enum { PANEL_FILL, PANEL_TOP, PANEL_BOT, PANEL_LEFT, PANEL_RIGHT, PANEL_TL, PANEL_TR, PANEL_BL, PANEL_BR };\n')
    f.write('/* per mode (relaxed, classic, expert): BG palettes 0-5, block palettes (OBJ 0-6) and colour count,\n'
            '   sky gradient (HBlank DMA), background layer tiles + map (BG2) */\n')
    f.write('typedef struct { const u16* bg_pal; const u16* block_pal; u8 ncolors; const u16* sky; const u32* layer_tiles; u16 layer_words; const u16* layer_map; } Theme;\n')
    f.write('extern const Theme themes[3];\n')

with open(OUT_C, 'w') as f:
    f.write('/* generated by tools/gen_gfx.py */\n#include "gfx_gen.h"\n')
    f.write(arr('obj_pal', 'u16', [v for p in obj_pal for v in p], 16, '0x{:04X}'))
    f.write(arr('font_tiles', 'u32', font))
    f.write(arr('font_panel_tiles', 'u32', font_panel))
    f.write(arr('bigdigit_tiles', 'u32', big))
    f.write(arr('panel_tiles', 'u32', panel))
    f.write(arr('base_tiles', 'u32', base_words))
    f.write(arr('logo_tiles', 'u32', logo_words))
    f.write(arr('logo_pal', 'u16', logo_colors, 16, '0x{:04X}'))
    for i, m in enumerate(MODES):
        f.write(arr(f'{m}_bg_pal', 'u16', mode_bg_pal[i], 16, '0x{:04X}').replace('const u16', 'static const u16'))
        f.write(arr(f'{m}_block_pal', 'u16', mode_block_pal[i], 16, '0x{:04X}').replace('const u16', 'static const u16'))
        f.write(arr(f'{m}_sky', 'u16', skies[i], 16, '0x{:04X}').replace('const u16', 'static const u16'))
        f.write(arr(f'{m}_layer_tiles', 'u32', layer_tiles[i]).replace('const u32', 'static const u32'))
        f.write(arr(f'{m}_layer_map', 'u16', [v | (5 << 12) for v in layer_maps[i]], 16, '0x{:04X}').replace('const u16', 'static const u16'))
    f.write('const Theme themes[3] = {\n')
    for i, m in enumerate(MODES):
        f.write(f'  {{ {m}_bg_pal, {m}_block_pal, {len(BLOCK_COLORS[m])}, {m}_sky, {m}_layer_tiles, {len(layer_tiles[i])}, {m}_layer_map }},\n')
    f.write('};\n')
print('font', len(font) // 8, 'tiles; logo', len(logo_words) // 16, 'tiles;', len(logo_colors), 'colours')
