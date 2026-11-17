#!/usr/bin/env python3
"""Pre-render STAK! block faces into GBA sprites and export 2D physics shapes.

Reads ../../models/pieces.glb (the same models the web build uses), renders the
front face of each block orthographically with lighting baked in, quantizes it to
a 16-colour shading ramp (palette-swappable per block colour) and writes:

  src/gen/pieces_gen.c / .h   sprite tiles, convex collision parts, mass props
  build/preview/*.png          previews for eyeballing

World units in the physics engine are "cells": 1 cell = 0.025 m = 12 px.

Collision parts are the models' convex collision meshes, flattened. A round
collision mesh becomes a true circle part (n = 1: centre in v[0], radius in
nrm[0][0]), so circles roll. The semicircle is cut into horizontal slabs.
"""
import os, sys, math
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
exec(open(os.path.join(HERE, 'glbload.py')).read().replace('__file__', repr(os.path.join(HERE, 'glbload.py'))))

CLASSIC = ['Plus', 'Rectangle', 'L', 'L-Reverse', 'Arrow', 'PlusCut', 'PeterPan', 'S', 'Z', 'T', 'Zig', 'Zag']
EXPERT = ['Circle', 'Semicircle', 'Lollipop', 'Seesaw']     # expert mode adds these
PIECES = CLASSIC + EXPERT
MAX_PARTS = 5                   # physics.h
CELL_M = 0.025
PX_PER_CELL = 12
SS = 4                     # supersampling
SPR = 64                   # sprite size (px)
RAMP = 8                   # shading levels -> palette idx 2..9
OUTLINE_IDX = 1
SPEC_IDX = 10


def raster_front(P, T, N, com_m):
    """Orthographic front render. Returns (mask, shade) arrays at SPR*SS resolution."""
    S = SPR * SS
    scale = PX_PER_CELL / CELL_M * SS        # supersampled px per metre
    # image coords: x right, y down; COM at the image centre
    X = (P[:, 0] - com_m[0]) * scale + S / 2
    Y = -(P[:, 1] - com_m[1]) * scale + S / 2
    Z = P[:, 2]
    zbuf = np.full((S, S), -1e9)
    nx = np.zeros((S, S)); ny = np.zeros((S, S)); nz = np.zeros((S, S))
    for t in T:
        a, b, c = t
        # triangle normal from vertex normals (average)
        n = N[a] + N[b] + N[c]
        n = n / (np.linalg.norm(n) + 1e-9)
        if n[2] < -0.05:
            continue
        xs = X[[a, b, c]]; ys = Y[[a, b, c]]; zs = Z[[a, b, c]]
        x0, x1 = int(max(0, math.floor(xs.min()))), int(min(S - 1, math.ceil(xs.max())))
        y0, y1 = int(max(0, math.floor(ys.min()))), int(min(S - 1, math.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(d) < 1e-12:
            continue
        l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
        l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
        l2 = 1 - l0 - l1
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any():
            continue
        z = l0 * zs[0] + l1 * zs[1] + l2 * zs[2]
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        upd = inside & (z > sub)
        sub[upd] = z[upd]
        nn = (l0[..., None] * N[a] + l1[..., None] * N[b] + l2[..., None] * N[c])
        nn /= np.linalg.norm(nn, axis=-1, keepdims=True) + 1e-9
        nx[y0:y1 + 1, x0:x1 + 1][upd] = nn[..., 0][upd]
        ny[y0:y1 + 1, x0:x1 + 1][upd] = nn[..., 1][upd]
        nz[y0:y1 + 1, x0:x1 + 1][upd] = nn[..., 2][upd]
    mask = zbuf > -1e8
    # The models' bevels are ~1mm, far below a pixel, so synthesize a chunky
    # pixel-art bevel from the silhouette: distance to the edge + edge normal.
    m = mask.astype(float)
    def blur(a, r):
        k = 2 * r + 1
        p = np.pad(a, r, mode='edge')
        c = np.cumsum(np.cumsum(np.pad(p, ((1, 0), (1, 0))), 0), 1)
        return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
    dist = np.zeros_like(m); cur = mask.copy()
    BEV = int(3.2 * SS)
    for i in range(BEV):
        dist += cur
        pad = np.pad(cur, 1)
        cur = cur & pad[:-2, 1:-1] & pad[2:, 1:-1] & pad[1:-1, :-2] & pad[1:-1, 2:]
    sm = blur(m, SS * 2)
    gy, gx = np.gradient(sm)
    gl = np.hypot(gx, gy) + 1e-9
    ex, ey = -gx / gl, gy / gl          # outward edge normal, y up
    L = np.array([-0.6, 0.8]); L /= np.linalg.norm(L)
    edge_light = ex * L[0] + ey * L[1]  # +1 lit edge, -1 shadow edge
    bevel = np.clip(1 - dist / BEV, 0, 1)
    S_ = mask.shape[0]
    yy, xx = np.mgrid[0:S_, 0:S_] / S_
    face = 0.58 + 0.36 * (0.5 - (0.55 * yy + 0.45 * xx))      # diagonal metallic sheen
    shade = face + np.where(bevel > 0.3, 0.55 * np.sign(edge_light) * np.minimum(1, np.abs(edge_light) * 1.6), 0)
    # glossy specular band across the upper-left of the face
    band = np.exp(-(((xx + yy) - 0.78) / 0.05) ** 2) * (1 - bevel)
    spec = band * 0.0 + (bevel > 0.35) * (edge_light > 0.8) * 1.0
    return mask, shade, spec


def downsample(mask, shade, spec):
    S = SPR
    m = mask.reshape(S, SS, S, SS).mean(axis=(1, 3))
    w = mask.astype(float)
    sh = (shade * w).reshape(S, SS, S, SS).sum(axis=(1, 3)) / np.maximum(w.reshape(S, SS, S, SS).sum(axis=(1, 3)), 1e-9)
    sp = (spec * w).reshape(S, SS, S, SS).sum(axis=(1, 3)) / np.maximum(w.reshape(S, SS, S, SS).sum(axis=(1, 3)), 1e-9)
    return m, sh, sp


def quantize(m, sh, sp):
    solid = m >= 0.5
    img = np.zeros(m.shape, dtype=np.uint8)
    lv = np.clip(((sh - 0.15) / 0.85) * RAMP, 0, RAMP - 1).astype(int)
    img[solid] = 2 + lv[solid]
    img[solid & (sp > 0.55)] = SPEC_IDX
    # 1px dark outline on the inside edge of the silhouette (Pokemon-style sprites)
    pad = np.pad(solid, 1)
    edge = solid & ~(pad[:-2, 1:-1] & pad[2:, 1:-1] & pad[1:-1, :-2] & pad[1:-1, 2:])
    img[edge] = OUTLINE_IDX
    return img, solid


def convex_hull(pts):
    pts = sorted(set(map(tuple, pts)))
    if len(pts) <= 2:
        return pts
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, hi = [], []
    for p in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 1e-9:
            lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(hi) >= 2 and cross(hi[-2], hi[-1], p) <= 1e-9:
            hi.pop()
        hi.append(p)
    return lo[:-1] + hi[:-1]   # CCW


def mass_props(P, T):
    """Area, centroid and polar moment (about centroid) of the front silhouette, in cells."""
    S = 512
    mn = P[:, :2].min(0) / CELL_M - 0.05; mx = P[:, :2].max(0) / CELL_M + 0.05
    span = (mx - mn).max()
    sc = S / span
    img = Image.new('L', (S, S), 0)
    from PIL import ImageDraw
    d = ImageDraw.Draw(img)
    for t in T:
        tri = P[t][:, :2] / CELL_M
        d.polygon([((v[0] - mn[0]) * sc, (v[1] - mn[1]) * sc) for v in tri], fill=255)
    a = np.array(img) > 127
    ys, xs = np.nonzero(a)
    cx = (xs + 0.5) / sc + mn[0]; cy = (ys + 0.5) / sc + mn[1]
    px_area = 1 / sc / sc
    area = len(xs) * px_area
    mx_, my_ = cx.mean(), cy.mean()
    I = (((cx - mx_) ** 2 + (cy - my_) ** 2).sum() + len(xs) / 6.0 / sc / sc) * px_area
    return area, np.array([mx_, my_]), I


def fx(v):
    return int(round(v * 65536))


def split_part(name, h):
    """A collision hull as physics parts: itself (<= 4 vertices), a circle
    [(cx, cy, r)] (a fine regular polygon), or for the semicircle five
    horizontal slabs under 12 points of its arc."""
    if len(h) <= 4:
        return [h]
    c = np.mean(h, axis=0)
    d = np.hypot(*(np.array(h) - c).T)
    if len(h) >= 12 and d.max() - d.min() < 0.03 * d.max():
        return [[(round(c[0], 4), round(c[1], 4), round(d.max(), 4))]]
    assert name == 'Semicircle', (name, len(h))
    hs = np.array(h)
    y0 = hs[:, 1].min(); x0, x1 = hs[:, 0].min(), hs[:, 0].max()
    cx, r = (x0 + x1) / 2, (x1 - x0) / 2
    lv = []
    for k in range(6):
        t = math.pi * k / 11
        lv.append((r * math.cos(t), y0 + r * math.sin(t)))
    parts = []
    for k in range(5):
        (xa, ya), (xb, yb) = lv[k], lv[k + 1]
        parts.append([(round(cx - xa, 4), round(ya, 4)), (round(cx + xa, 4), round(ya, 4)),
                      (round(cx + xb, 4), round(yb, 4)), (round(cx - xb, 4), round(yb, 4))])
    return parts


def main():
    out_c = os.path.join(ROOT, 'src', 'gen', 'pieces_gen.c')
    out_h = os.path.join(ROOT, 'src', 'gen', 'pieces_gen.h')
    prev = os.path.join(ROOT, 'build', 'preview')
    os.makedirs(os.path.dirname(out_c), exist_ok=True); os.makedirs(prev, exist_ok=True)
    tiles_all = []; defs = []
    for name in PIECES:
        pc = pieces[name]
        mi = pc['model'][0]
        P, T = mesh(mi)
        N = acc(j['meshes'][mi]['primitives'][0]['attributes']['NORMAL']).astype(float)
        area, com, inertia = mass_props(P, T)
        # Affine sprites turn about their centre, so the sprite is centred on the
        # COM unless the piece then doesn't fit (the lollipop): it is centred on
        # its box instead and the game places it with the offset turned too.
        xy = P[:, :2] / CELL_M - com
        off = np.zeros(2)
        if np.hypot(xy[:, 0], xy[:, 1]).max() * PX_PER_CELL > SPR / 2 - 2:
            off = (xy.min(0) + xy.max(0)) / 2
        mask, shade, spec = raster_front(P, T, N, (com + off) * CELL_M)
        m, sh, sp = downsample(mask, shade, spec)
        img, solid = quantize(m, sh, sp)
        ys, xs = np.nonzero(solid)
        assert xs.min() > 0 and ys.min() > 0 and xs.max() < SPR - 1 and ys.max() < SPR - 1, name
        r = math.sqrt(((xs - SPR / 2 + 0.5) ** 2 + (ys - SPR / 2 + 0.5) ** 2).max())
        assert r < SPR / 2, (name, r)
        small = r < SPR / 4 - 0.5          # fits a 32x32 sprite (a quarter of the tiles)
        if small:
            img = img[SPR // 4:SPR * 3 // 4, SPR // 4:SPR * 3 // 4]
        # collision parts: project to xy, hull, in cells relative to COM
        parts = []
        for ci in pc['col']:
            Q, _ = mesh(ci)
            h = convex_hull([(round(q[0] / CELL_M - com[0], 4), round(q[1] / CELL_M - com[1], 4)) for q in Q])
            parts.extend(split_part(name, h))
        assert len(parts) <= MAX_PARTS, (name, len(parts))
        # bounding radius (cells)
        rad = max(math.hypot(p[0][0], p[0][1]) + p[0][2] if len(p) == 1 else max(math.hypot(*v) for v in p) for p in parts)
        # tiles (1D mapping, 8x8 4bpp)
        tiles = []
        for ty in range(img.shape[0] // 8):
            for tx in range(img.shape[1] // 8):
                blk = img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
                for row in blk:
                    w = 0
                    for k in range(8):
                        w |= int(row[k]) << (4 * k)
                    tiles.append(w)
        tiles_all.append(tiles)
        # half-size tray icon (32x32) rendered from the same image
        defs.append(dict(name=name, area=area, inertia=inertia, parts=parts, rad=rad, off=off, small=small,
                         top=(SPR / 2 - ys.min()) / PX_PER_CELL + off[1], bottom=(ys.max() + 1 - SPR / 2) / PX_PER_CELL - off[1]))
        # preview with a sample colour
        pal = np.zeros((16, 3), np.uint8)
        base = np.array([0x00, 0xD1, 0xFF])
        for k in range(RAMP):
            t = k / (RAMP - 1)
            c = base * (0.35 + 0.75 * t) + 255 * max(0, t - 0.6) * 0.6
            pal[2 + k] = np.clip(c, 0, 255)
        pal[1] = base * 0.22; pal[SPEC_IDX] = (255, 255, 255); pal[0] = (30, 30, 46)
        Image.fromarray(pal[img]).resize((img.shape[1] * 4, img.shape[0] * 4), Image.NEAREST).save(os.path.join(prev, f'{name}.png'))
        print(f'{name:10s} area={area:.3f} I={inertia:.3f} parts={len(parts)} rad={rad:.2f} r_px={r:.1f}'
              f'{" 32x32" if small else ""}{f" offset {off[0]:.2f},{off[1]:.2f}" if off.any() else ""}')

    with open(out_h, 'w') as f:
        f.write('/* generated by tools/gen_pieces.py - do not edit */\n#pragma once\n#include "../gba.h"\n\n')
        maxv = max(len(p) for d in defs for p in d['parts'])
        assert maxv <= 4, 'physics.h MAX_VERTS is 4: split parts with more vertices'
        ntiles = sum(len(t) // 8 for t in tiles_all)
        f.write(f'#define PIECE_COUNT {len(PIECES)}\n#define PIECE_CLASSIC_COUNT {len(CLASSIC)}\n'
                f'#define PIECE_MAX_PARTS {max(len(d["parts"]) for d in defs)}\n#define PIECE_MAX_VERTS {maxv}\n'
                f'#define PIECE_TILES {ntiles}          /* 4bpp OBJ tiles for all the sprites */\n\n')
        f.write('/* n = 1: a circle, centre v[0], radius nrm[0][0] */\n')
        f.write('typedef struct { u8 n; s32 v[PIECE_MAX_VERTS][2]; s32 nrm[PIECE_MAX_VERTS][2]; } PiecePart;\n')
        f.write('typedef struct {\n  const char* name;\n  s32 mass, inertia;      /* Q16, cells^2 / cells^4 */\n'
                '  s32 radius;             /* Q16 bounding radius from COM */\n  s32 top, bottom;        /* Q16 extent above/below COM at angle 0 */\n'
                '  s32 sox, soy;           /* Q16 sprite centre relative to the COM at angle 0 */\n'
                '  u16 tile; u8 small;     /* first OBJ tile; 32x32 sprite instead of 64x64 */\n'
                '  u8 nparts;\n  PiecePart parts[PIECE_MAX_PARTS];\n} PieceDef;\n\n')
        f.write('extern const PieceDef piece_defs[PIECE_COUNT];\nextern const u32 piece_tiles[PIECE_TILES * 8];\n')
    with open(out_c, 'w') as f:
        f.write('/* generated by tools/gen_pieces.py - do not edit */\n#include "pieces_gen.h"\n\n')
        f.write('const PieceDef piece_defs[PIECE_COUNT] = {\n')
        tile = 0
        for d, t in zip(defs, tiles_all):
            f.write(f'  {{ "{d["name"]}", {fx(d["area"])}, {fx(d["inertia"])}, {fx(d["rad"])}, {fx(d["top"])}, {fx(d["bottom"])}, '
                    f'{fx(d["off"][0])}, {fx(d["off"][1])}, {tile}, {int(d["small"])}, {len(d["parts"])}, {{\n')
            tile += len(t) // 8
            for p in d['parts']:
                if len(p) == 1:      # circle
                    (cx, cy, r), = p
                    f.write(f'    {{ 1, {{ {{{fx(cx)},{fx(cy)}}} }}, {{ {{{fx(r)},0}} }} }},\n')
                    continue
                vs = ', '.join(f'{{{fx(v[0])},{fx(v[1])}}}' for v in p)
                ns = []
                for k in range(len(p)):
                    a, b = p[k], p[(k + 1) % len(p)]
                    ex, ey = b[0] - a[0], b[1] - a[1]
                    l = math.hypot(ex, ey)
                    ns.append(f'{{{fx(ey / l)},{fx(-ex / l)}}}')
                f.write(f'    {{ {len(p)}, {{ {vs} }}, {{ {", ".join(ns)} }} }},\n')
            f.write('  } },\n')
        f.write('};\n\nconst u32 piece_tiles[PIECE_TILES * 8] __attribute__((aligned(4))) = {\n')
        for t in tiles_all:
            for i in range(0, len(t), 16):
                f.write('  ' + ','.join(f'0x{w:08X}' for w in t[i:i + 16]) + ',\n')
        f.write('};\n')


if __name__ == '__main__':
    main()
