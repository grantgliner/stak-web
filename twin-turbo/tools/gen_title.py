#!/usr/bin/env python3
"""Title and ending screens as Mode 4 (8bpp bitmap) images.

The backdrop riffs on the TURBO EP cover: a glossy pink car-hood close-up
with a double-bubble headlight and pinstripes. The TWIN TURBO logo is heavy
italic lettering, extruded 8 px down-right in deep magenta chrome, with an
iridescent cyan-cream-pink face (the cover's lettering colours), a bevel
highlight and a thick dark outline."""
import os, sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from gfxlib import CWriter, rgb15, hexc

W, H = 240, 160
FONT_LOGO = '/usr/share/fonts/opentype/inter/InterDisplay-BlackItalic.otf'
FONT_SUB = '/usr/share/fonts/opentype/inter/InterDisplay-ExtraBoldItalic.otf'
rng = np.random.default_rng(11)


def lerp(a, b, t):
    return a + (b - a) * t


def gradient(h, keys):
    out = np.zeros((h, 3))
    for y in range(h):
        t = y / (h - 1)
        for (t0, c0), (t1, c1) in zip(keys, keys[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0) if t1 > t0 else 0
                out[y] = lerp(np.array(hexc(c0), float), np.array(hexc(c1), float), k)
                break
    return out


def hood_backdrop(seed=0, dark=False):
    """glossy pink car hood with a bubble headlight, cover-style"""
    yy, xx = np.mgrid[0:H, 0:W].astype(float)
    g = gradient(H, [(0, '#f2668f'), (0.45, '#ff9fbc'), (0.8, '#f7b6dc'), (1, '#b84ab8')])
    img = g[:, None, :].repeat(W, 1)
    # broad diagonal sheen bands
    for (off, wdt, a) in ((-40, 34, 0.30), (60, 18, 0.22), (150, 50, 0.18)):
        d = (xx * 0.55 + yy) - (off + 120)
        band = np.exp(-(d / wdt) ** 2) * a
        img = img + (255 - img) * band[..., None]
    # pinstripes sweeping from upper-left (red / white pairs)
    for k, (c, off) in enumerate((('#c8102e', 0), ('#ffffff', 3), ('#c8102e', 6))):
        cc = np.array(hexc(c), float)
        for x in range(W):
            y = int(round(-30 + x * 0.42 + off + 0.0009 * (x - 60) ** 2))
            if 0 <= y < H:
                img[y, x] = cc
            y2 = int(round(150 - x * 0.62 + off))
            if 0 <= y2 < H and x > 150:
                img[y2, x] = cc
    # the bubble headlight (two lobes), lower half
    # metaball union of a big lower-left lobe and a smaller, higher right
    # lobe: a smooth waist between them, like the cover's headlight
    def field(cx, cy, r):
        return r * r / np.maximum(1.0, (xx - cx) ** 2 + ((yy - cy) * 1.25) ** 2)
    fld = field(64, 150, 50) + field(166, 122, 34)
    inside = fld >= 1.0
    rim = (fld >= 0.78) & ~inside
    glow = (fld >= 0.62) & ~inside & ~rim
    def lobe(cx, cy, rx, ry):
        return ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2
    # lens: glassy pink with a bright upper crescent and inner reflection
    lens = np.zeros((H, W, 3))
    lg = gradient(H, [(0, '#fff4f8'), (0.55, '#ffe0ee'), (0.75, '#ffb8d8'), (1, '#e070b8')])
    lens[:] = lg[:, None, :]
    cres = inside & (yy < 150 - 0.45 * xx - 8 + 0.002 * (xx - 110) ** 2)
    lens[cres] = lens[cres] * 0.4 + np.array([255, 250, 252]) * 0.6
    inner = (lobe(70, 162, 40, 16) <= 1) | (lobe(170, 134, 22, 10) <= 1)
    lens[inner & inside] = lens[inner & inside] * 0.6 + np.array(hexc('#ff5ca8'), float) * 0.4
    img[inside] = lens[inside]
    img[glow] = img[glow] * 0.6 + np.array([255, 255, 255]) * 0.4
    img[rim] = np.array(hexc('#2a0a28'), float)
    # sparkle on the lens
    sx, sy = 40, 120
    for r in range(-12, 13):
        a = max(0, 1 - abs(r) / 12) ** 1.5
        for (x, y) in ((sx + r, sy), (sx, sy + r)):
            if 0 <= x < W and 0 <= y < H:
                img[y, x] = img[y, x] + (255 - img[y, x]) * a
    # grain, like the cover print
    img = img + rng.normal(0, 4.5, img.shape)
    if dark:
        img = img * 0.55 + np.array([40, 10, 50]) * 0.45
    return np.clip(img, 0, 255)


def text_mask(text, font_path, size, skew=0.0, track=0):
    f = ImageFont.truetype(font_path, size)
    widths = [f.getbbox(ch)[2] - f.getbbox(ch)[0] for ch in text]
    total = sum(f.getlength(ch) for ch in text) + track * (len(text) - 1) + size
    im = Image.new('L', (int(total) + 40, size * 2), 0)
    d = ImageDraw.Draw(im)
    x = 10
    for ch in text:
        d.text((x, 0), ch, font=f, fill=255)
        x += f.getlength(ch) + track + (size * 0.12 if ch == ' ' else 0)
    a = np.array(im) > 110
    ys, xs = np.nonzero(a)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return a


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r + r:
                out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def logo_layer(text, size, depth=8, track=1):
    """returns RGBA float image of 3D lettering"""
    face = text_mask(text, FONT_LOGO, size, track=track)
    h, w = face.shape
    pad = depth + 6
    F = np.zeros((h + pad * 2, w + pad * 2), bool)
    F[pad:pad + h, pad:pad + w] = face
    Hh, Ww = F.shape
    rgb = np.zeros((Hh, Ww, 3))
    alpha = np.zeros((Hh, Ww), bool)
    # extrusion: step down-right, dark magenta chrome getting deeper
    ext = np.zeros_like(F)
    for k in range(depth, 0, -1):
        m = np.roll(np.roll(F, k, 0), k // 2 + k // 3, 1)
        t = k / depth
        col = lerp(np.array(hexc('#d02e8c'), float), np.array(hexc('#4a0e5a'), float), t)
        # chrome banding on the extrusion
        if k in (2, 3):
            col = lerp(col, np.array([255, 190, 230], float), 0.35)
        rgb[m] = col
        ext |= m
    alpha |= ext
    # face: iridescent vertical gradient (cover lettering: cyan -> cream -> pink)
    ys = np.nonzero(F.any(1))[0]
    y0, y1 = ys.min(), ys.max()
    keys = [(0, '#8eeaff'), (0.38, '#e8fff4'), (0.55, '#fff2c4'), (0.78, '#ffc0d8'), (1, '#ff7ab8')]
    g = gradient(y1 - y0 + 1, keys)
    for y in range(y0, y1 + 1):
        rgb[y][F[y]] = g[y - y0]
    # bevel: top-left edges brighter, bottom-right edges tinted
    up = F & ~np.roll(F, 1, 0)
    left = F & ~np.roll(F, 1, 1)
    br = F & (~np.roll(F, -1, 0) | ~np.roll(F, -1, 1))
    rgb[up | left] = rgb[up | left] * 0.3 + 255 * 0.7
    rgb[br] = rgb[br] * 0.75 + np.array(hexc('#ff5aa8'), float) * 0.25
    # horizontal shine streak through the face
    sy = int(y0 + (y1 - y0) * 0.3)
    for y in (sy, sy + 1):
        rgb[y][F[y]] = rgb[y][F[y]] * 0.3 + 255 * 0.7
    alpha |= F
    # thick dark outline around face + extrusion
    out = dilate(alpha, 2) & ~alpha
    rgb[out] = np.array(hexc('#1c0620'), float)
    # thin inner line between face and extrusion
    edge = ext & ~F & (np.roll(F, 1, 0) | np.roll(F, 1, 1) | np.roll(F, -1, 1))
    rgb[edge] = rgb[edge] * 0.6
    alpha |= out
    return rgb, alpha


def paste(img, layer, x, y):
    rgb, a = layer
    h, w = a.shape
    for yy in range(h):
        for xx in range(w):
            if a[yy, xx] and 0 <= y + yy < H and 0 <= x + xx < W:
                img[y + yy, x + xx] = rgb[yy, xx]


def small_text(text, size, font=FONT_SUB, keys=None, outline='#1c0620', track=0):
    m = text_mask(text, font, size, track=track)
    pad = 3
    F = np.zeros((m.shape[0] + 2 * pad, m.shape[1] + 2 * pad), bool)
    F[pad:-pad, pad:-pad] = m
    rgb = np.zeros(F.shape + (3,))
    keys = keys or [(0, '#9ef0ff'), (0.5, '#fff6d8'), (1, '#ffb4d0')]
    # horizontal iridescent gradient like the cover's artist names
    gx = gradient(F.shape[1], keys)
    rgb[:] = gx[None, :, :]
    o = dilate(F, 1) & ~F
    rgb[o] = np.array(hexc(outline), float)
    return rgb, F | o


def quantize(img, ncol=240, reserve=16):
    """median-cut to ncol colours; palette index 0 is kept for the backdrop's
    darkest colour, the last `reserve` slots stay free for runtime use"""
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    q = im.quantize(colors=ncol - reserve, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette()[:3 * (ncol - reserve)]
    idx = np.array(q)
    cols = [tuple(pal[i * 3:i * 3 + 3]) for i in range(ncol - reserve)]
    return idx, cols


def build_title():
    img = hood_backdrop()
    twin = logo_layer('TWIN', 36, depth=7, track=2)
    turbo = logo_layer('TURBO', 46, depth=9, track=1)
    tw = twin[1].shape[1]; tb = turbo[1].shape[1]
    paste(img, twin, (W - tb) // 2 - 6, 6)
    paste(img, turbo, (W - tb) // 2 + 2, 38)
    sub = small_text('that franco × tripleset', 11, track=0)
    paste(img, sub, (W - sub[1].shape[1]) // 2, 98)
    return img


def build_end():
    img = hood_backdrop(dark=True)
    t1 = logo_layer('THANKS', 24, depth=5, track=1)
    paste(img, t1, (W - t1[1].shape[1]) // 2, 4)
    t2 = small_text('for playing TWIN TURBO', 10)
    paste(img, t2, (W - t2[1].shape[1]) // 2, 38)
    t3 = small_text('that franco × tripleset', 11)
    paste(img, t3, (W - t3[1].shape[1]) // 2, 118)
    t4 = small_text('TURBO EP', 13, keys=[(0, '#ffffff'), (1, '#ffd0e4')])
    paste(img, t4, (W - t4[1].shape[1]) // 2, 134)
    return img


def main():
    out = CWriter(os.path.join(ROOT, 'src/gen/title_gen.c'), os.path.join(ROOT, 'src/gen/title_gen.h'), 'tools/gen_title.py')
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    for name, fn in (('title', build_title), ('ending', build_end)):
        img = fn()
        idx, cols = quantize(img)
        pal = [rgb15(c) for c in cols] + [0] * (256 - len(cols))
        out.u16('%s_pal' % name, pal)
        words = []
        flat = idx.reshape(-1).astype(np.uint32)
        for i in range(0, len(flat), 4):
            words.append(int(flat[i]) | int(flat[i + 1]) << 8 | int(flat[i + 2]) << 16 | int(flat[i + 3]) << 24)
        out.u32('%s_bitmap' % name, words)
        rgb = np.array([cols[i] for i in flat]).reshape(H, W, 3).astype(np.uint8)
        Image.fromarray(rgb).resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(ROOT, 'build/%s.png' % name))
    out.close()


if __name__ == '__main__':
    main()
