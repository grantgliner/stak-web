#!/usr/bin/env python3
"""Generate the N64 port's assets into src/gen/assets_gen.c / .h.

  * Piece meshes: the outline of each block in models/pieces.glb (the web
    version's models), simplified and extruded into a low-poly bevelled prism.
    Coordinates are cells (1 cell = 2.5 cm) relative to the piece's centre of
    mass, matching the physics engine's bodies.
  * The base slab, and the gloved-hand cursor (open and grabbing).
  * Fonts: Nunito Black (the web version's typeface), as 4-bit alpha glyphs.
  * The STAK! logo, as RGBA5551 + 8-bit alpha.

Needs Python 3 with numpy, Pillow, shapely, mapbox_earcut and fonttools.
"""
import os, sys, math, io
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import Polygon, MultiPolygon
from shapely.ops import unary_union
import mapbox_earcut as earcut

HERE = os.path.dirname(os.path.abspath(__file__))
N64 = os.path.normpath(os.path.join(HERE, '..'))
REPO = os.path.normpath(os.path.join(N64, '..'))
GBA_TOOLS = os.path.join(REPO, 'gba', 'tools')

# reuse the GBA tool's GLB loader and mass properties (same COM as the physics)
src = open(os.path.join(GBA_TOOLS, 'gen_pieces.py')).read()
src = src.replace("os.path.dirname(os.path.abspath(__file__))", repr(GBA_TOOLS))
g = {'__name__': 'gen_pieces_lib', '__file__': os.path.join(GBA_TOOLS, 'gen_pieces.py')}
exec(compile(src, 'gen_pieces.py', 'exec'), g)
PIECES, pieces, mesh, mass_props, CELL_M = g['PIECES'], g['pieces'], g['mesh'], g['mass_props'], g['CELL_M']

OUT_C = os.path.join(N64, 'src', 'gen', 'assets_gen.c')
OUT_H = os.path.join(N64, 'src', 'gen', 'assets_gen.h')

meshes = []   # (name, verts, tris, normals, mats)


def ccw(poly):
    a = 0
    for i in range(len(poly)):
        x0, y0 = poly[i]; x1, y1 = poly[(i + 1) % len(poly)]
        a += x0 * y1 - x1 * y0
    return poly if a > 0 else poly[::-1]


def inset(poly, d):
    """Offset a CCW polygon inwards by d with mitred corners (1:1 vertices)."""
    n = len(poly); out = []
    P = np.array(poly)
    for i in range(n):
        a, b, c = P[i - 1], P[i], P[(i + 1) % n]
        e1 = b - a; e2 = c - b
        n1 = np.array([-e1[1], e1[0]]) / np.linalg.norm(e1)
        n2 = np.array([-e2[1], e2[0]]) / np.linalg.norm(e2)
        m = n1 + n2
        if np.linalg.norm(m) < 1e-6:
            m = n1
        m = m / np.linalg.norm(m)
        k = d / max(0.4, float(np.dot(m, n1)))
        out.append(tuple(b + m * k))
    return out


class MeshB:
    def __init__(self):
        self.v = []; self.t = []; self.m = []

    def vert(self, p):
        self.v.append(tuple(float(x) for x in p)); return len(self.v) - 1

    def tri(self, a, b, c, mat=0):
        self.t.append((a, b, c)); self.m.append(mat)

    def quad(self, a, b, c, d, mat=0):
        self.tri(a, b, c, mat); self.tri(a, c, d, mat)

    def box(self, center, size, rot=np.eye(3), mat=0, origin=(0, 0, 0)):
        """Axis box of `size` centred at `center` in a frame rotated by rot about origin."""
        cx, cy, cz = center; sx, sy, sz = [s / 2 for s in size]
        c = []
        for k in range(8):
            p = np.array([cx + (sx if k & 1 else -sx), cy + (sy if k & 2 else -sy), cz + (sz if k & 4 else -sz)])
            c.append(self.vert(rot @ p + np.array(origin)))
        # faces (outward CCW)
        self.quad(c[0], c[2], c[3], c[1], mat)   # -z
        self.quad(c[4], c[5], c[7], c[6], mat)   # +z
        self.quad(c[0], c[1], c[5], c[4], mat)   # -y
        self.quad(c[2], c[6], c[7], c[3], mat)   # +y
        self.quad(c[0], c[4], c[6], c[2], mat)   # -x
        self.quad(c[1], c[3], c[7], c[5], mat)   # +x

    def prism(self, outline, z0, z1, bevel, mat=0, back=False, origin=(0, 0, 0), rot=np.eye(3)):
        """Extrude a 2D outline (CCW) from z0 to z1 with a front bevel (and a back one)."""
        o = np.array(origin)
        def V(x, y, z): return self.vert(rot @ np.array([x, y, z]) + o)
        outline = ccw(list(outline))
        ins = inset(outline, bevel)
        n = len(outline)
        zb = z0 + (bevel if back else 0)
        lo = [V(x, y, zb) for x, y in outline]
        hi = [V(x, y, z1 - bevel) for x, y in outline]
        cap = [V(x, y, z1) for x, y in ins]
        for i in range(n):
            j = (i + 1) % n
            self.quad(lo[i], lo[j], hi[j], hi[i], mat)
            self.quad(hi[i], hi[j], cap[j], cap[i], mat)
        tri = earcut.triangulate_float32(np.array(ins, np.float32), np.array([n], np.uint32)).reshape(-1, 3)
        for a, b, c in tri:
            # earcut keeps the input orientation (CCW here, so facing +z)
            p = [ins[a], ins[b], ins[c]]
            cr = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])
            if cr < 0: b, c = c, b
            self.tri(cap[a], cap[b], cap[c], mat)
        if back:
            bcap = [V(x, y, z0) for x, y in ins]
            for i in range(n):
                j = (i + 1) % n
                self.quad(bcap[i], bcap[j], lo[j], lo[i], mat)
            for a, b, c in tri:
                p = [ins[a], ins[b], ins[c]]
                cr = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])
                if cr > 0: b, c = c, b
                self.tri(bcap[a], bcap[b], bcap[c], mat)

    def finish(self, name):
        V = np.array(self.v)
        N = []
        for a, b, c in self.t:
            nrm = np.cross(V[b] - V[a], V[c] - V[a])
            l = np.linalg.norm(nrm)
            N.append(tuple(nrm / l) if l > 1e-9 else (0.0, 0.0, 1.0))
        meshes.append((name, self.v, self.t, N, self.m))
        return len(meshes) - 1


def rotx(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def roty(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rotz(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


# ------------------------------------------------------------------ pieces
piece_info = []
for name in PIECES:
    pc = pieces[name]
    P, T = mesh(pc['model'][0])
    area, com, inertia = mass_props(P, T)
    polys = []
    for t in T:
        tri = P[t][:, :2] / CELL_M - com
        pg = Polygon(tri)
        if pg.area > 1e-7:
            polys.append(pg.buffer(0.002))
    sil = unary_union(polys)
    if isinstance(sil, MultiPolygon):
        sil = max(sil.geoms, key=lambda q: q.area)
    sil = sil.buffer(-0.002).simplify(0.07, preserve_topology=True)
    outline = list(sil.exterior.coords)[:-1]
    mb = MeshB()
    mb.prism(outline, -0.5, 0.5, 0.13, back=True)
    idx = mb.finish('piece_' + name.replace('-', '_'))
    piece_info.append((name, idx, len(outline)))
    print(f'{name:10s} outline {len(outline):3d} verts, mesh {len(mb.v)} v {len(mb.t)} t')

# ------------------------------------------------------------------ base slab
def rounded_rect(x0, y0, x1, y1, r, seg=3):
    pts = []
    corners = [(x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)]
    for cx, cy, a0 in corners:
        for k in range(seg + 1):
            a = math.radians(a0 + 90 * k / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

mb = MeshB()
mb.prism(rounded_rect(-4.1, -2.0, 4.1, 0.0, 0.3, 2), -2.2, 2.2, 0.22, mat=0)
# a bright lip along the top front edge, like the web base's chrome strip
mb.box((0, -0.32, 2.21), (7.6, 0.12, 0.02), mat=1)
BASE = mb.finish('base')

# ------------------------------------------------------------------ gloved hand
# Seen from behind (back of the hand toward the camera), fingers pointing up.
# Units: cells. Origin: the middle of the palm. mat 0 glove, 1 cuff, 2 cuff stripe, 3 seams
def hand(closed):
    """About 170 triangles: it is drawn every frame."""
    mb = MeshB()
    # palm (slightly rounded: a bevelled prism in x/y, thickness along z)
    palm = rounded_rect(-0.62, -0.55, 0.62, 0.55, 0.2, 1)
    mb.prism(palm, -0.28, 0.28, 0.1, back=True)
    # back-of-hand seams: three raised strips, like a cartoon glove
    for x in (-0.25, 0.0, 0.25):
        a = mb.vert((x - 0.03, -0.22, 0.29)); b = mb.vert((x + 0.03, -0.22, 0.29))
        c = mb.vert((x + 0.03, 0.32, 0.29)); d = mb.vert((x - 0.03, 0.32, 0.29))
        mb.quad(a, b, c, d, 3)
    # fingers: x position, length scale, splay
    fingers = [(-0.45, 0.85, 0.16), (-0.15, 1.0, 0.05), (0.15, 0.97, -0.05), (0.45, 0.8, -0.16)]
    for fxp, ln, spl in fingers:
        base = np.array([fxp, 0.5, 0.0])
        if not closed:
            r1 = rotz(spl) @ rotx(-0.08)
            seg = ln * 0.98
            mb.box((0, seg / 2, 0), (0.27, seg, 0.29), rot=r1, origin=base)
        else:
            # curled over: we see the knuckles above the palm, the rest is hidden behind it
            r1 = rotx(-1.0)
            seg1 = ln * 0.5
            mb.box((0, seg1 / 2, 0), (0.29, seg1, 0.33), rot=r1, origin=base + np.array([0, -0.05, 0.02]))
    # thumb on the left (it's a right hand seen from behind)
    tb = np.array([-0.6, -0.15, 0.0])
    if not closed:
        r = rotz(0.95)
        mb.box((0, 0.3, 0), (0.3, 0.6, 0.32), rot=r, origin=tb)
        t2 = tb + r @ np.array([0, 0.6, 0])
        mb.box((0, 0.2, 0), (0.27, 0.42, 0.3), rot=rotz(0.55), origin=t2)
    else:
        r = rotz(-0.35) @ rotx(-0.9)
        mb.box((0, 0.3, 0), (0.3, 0.62, 0.33), rot=r, origin=tb + np.array([0.08, 0.0, -0.05]))
    # cuff: octagonal ring below the palm, with a stripe round it
    oct8 = lambda rx, ry: [(rx * math.cos(math.pi / 8 + k * math.pi / 4), ry * math.sin(math.pi / 8 + k * math.pi / 4)) for k in range(8)]
    mb.prism(oct8(0.78, 0.36), 0.42, 0.86, 0.0, mat=1, back=True, rot=rotx(math.pi / 2), origin=(0, 0, 0))
    ring = oct8(0.8, 0.38)
    R = rotx(math.pi / 2)
    lo = [mb.vert(R @ np.array([x, y, 0.62])) for x, y in ring]
    hi = [mb.vert(R @ np.array([x, y, 0.74])) for x, y in ring]
    for i in range(8):
        j = (i + 1) % 8
        mb.quad(lo[i], lo[j], hi[j], hi[i], 2)
    return mb

# the cuff prism above is built along z then turned so it runs along y
HAND_OPEN = hand(False).finish('hand_open')
HAND_GRAB = hand(True).finish('hand_grab')

# ------------------------------------------------------------------ fonts
def font_path():
    for p in [os.path.join(N64, 'tools', 'Nunito-Black.ttf')]:
        if os.path.exists(p): return p
    raise SystemExit('Nunito Black TTF not found (put it at tools/Nunito-Black.ttf)')

fonts = []   # (name, glyphs dict ch -> (w,h,xo,yo,adv,data4), line height)
def make_font(name, size, chars):
    f = ImageFont.truetype(font_path(), size)
    asc, desc = f.getmetrics()
    gl = {}
    for ch in chars:
        bb = f.getbbox(ch)
        adv = int(round(f.getlength(ch)))
        if bb[2] <= bb[0] or bb[3] <= bb[1]:
            gl[ch] = (0, 0, 0, 0, adv, b''); continue
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-bb[0], -bb[1]), ch, font=f, fill=255)
        a = np.array(im, dtype=np.uint16)
        a4 = (a * 15 + 127) // 255
        gl[ch] = (w, h, bb[0], bb[1], adv, a4)
    fonts.append((name, gl, asc + desc, asc))

ASCII = ''.join(chr(c) for c in range(32, 127))
make_font('font_big', 40, '0123456789+-,')
make_font('font_med', 15, ASCII)
make_font('font_small', 10, ASCII)
make_font('font_title', 22, ASCII)

# ------------------------------------------------------------------ logo
def logo(width):
    im = Image.open(os.path.join(REPO, 'images', 'stak-logo.png')).convert('RGBA')
    h = round(im.size[1] * width / im.size[0])
    im = im.resize((width, h), Image.LANCZOS)
    a = np.array(im).astype(int)
    return width, h, a

LOGO_W, LOGO_H, logo_px = logo(232)

# ------------------------------------------------------------------ write C
def carr(name, ctype, data, per=12, fmt='{}'):
    s = f'const {ctype} {name}[{len(data)}] = {{\n'
    for i in range(0, len(data), per):
        s += '  ' + ', '.join(fmt.format(x) for x in data[i:i + per]) + ',\n'
    return s + '};\n'

with open(OUT_H, 'w') as f:
    f.write('/* generated by tools/gen_assets.py - do not edit */\n#pragma once\n#include <stdint.h>\n\n')
    f.write('typedef struct { uint16_t nv, nt; const float (*v)[3]; const uint16_t (*t)[3]; const float (*n)[3]; const uint8_t* mat; } Mesh;\n')
    f.write('typedef struct { uint8_t w, h; int8_t xo, yo; uint8_t adv; uint32_t off; } Glyph;\n')
    f.write('typedef struct { const Glyph* g; const uint8_t* data; uint8_t first, count, line, ascent; } Font;\n\n')
    f.write(f'#define MESH_COUNT {len(meshes)}\n#define MESH_PIECE0 0\n#define MESH_BASE {BASE}\n#define MESH_HAND_OPEN {HAND_OPEN}\n#define MESH_HAND_GRAB {HAND_GRAB}\n')
    f.write('extern const Mesh meshes[MESH_COUNT];\n')
    for name, *_ in fonts:
        f.write(f'extern const Font {name};\n')
    f.write(f'\n#define LOGO_W {LOGO_W}\n#define LOGO_H {LOGO_H}\nextern const uint16_t logo_rgba[LOGO_W * LOGO_H];\nextern const uint8_t logo_alpha[LOGO_W * LOGO_H];\n')

with open(OUT_C, 'w') as f:
    f.write('/* generated by tools/gen_assets.py - do not edit */\n#include "assets_gen.h"\n\n')
    for name, V, T, N, M in meshes:
        f.write(f'static const float {name}_v[{len(V)}][3] = {{\n')
        for v in V: f.write('  {%.4ff, %.4ff, %.4ff},\n' % v)
        f.write('};\n')
        f.write(f'static const uint16_t {name}_t[{len(T)}][3] = {{\n')
        for t in T: f.write('  {%d, %d, %d},\n' % tuple(t))
        f.write('};\n')
        f.write(f'static const float {name}_n[{len(N)}][3] = {{\n')
        for n in N: f.write('  {%.4ff, %.4ff, %.4ff},\n' % tuple(n))
        f.write('};\n')
        f.write(carr(f'static {name}_m'.replace('static ', ''), 'uint8_t', M, 32).replace('const uint8_t ' + name, 'static const uint8_t ' + name))
    f.write('const Mesh meshes[MESH_COUNT] = {\n')
    for name, V, T, N, M in meshes:
        f.write(f'  {{ {len(V)}, {len(T)}, {name}_v, {name}_t, {name}_n, {name}_m }},\n')
    f.write('};\n\n')
    for name, gl, line, asc in fonts:
        chars = sorted(gl.keys())
        first = ord(chars[0]); last = ord(chars[-1])
        data = bytearray(); recs = []
        for c in range(first, last + 1):
            ch = chr(c)
            if ch not in gl:
                recs.append((0, 0, 0, 0, 0, 0)); continue
            w, h, xo, yo, adv, a4 = gl[ch]
            off = len(data)
            if w:
                flat = np.array(a4).reshape(-1)
                if len(flat) % 2: flat = np.append(flat, 0)
                for i in range(0, len(flat), 2):
                    data.append(int(flat[i]) << 4 | int(flat[i + 1]))
            recs.append((w, h, xo, yo, adv, off))
        f.write(f'static const Glyph {name}_g[{len(recs)}] = {{\n')
        for r in recs: f.write('  {%d, %d, %d, %d, %d, %d},\n' % r)
        f.write('};\n')
        f.write(carr(f'{name}_d', 'uint8_t', list(data), 24).replace(f'const uint8_t {name}_d', f'static const uint8_t {name}_d'))
        f.write(f'const Font {name} = {{ {name}_g, {name}_d, {first}, {last - first + 1}, {line}, {asc} }};\n\n')
    rgba = []; al = []
    for y in range(LOGO_H):
        for x in range(LOGO_W):
            r, gg, b, a = logo_px[y, x]
            if a > 0:   # un-premultiply-ish: colours are straight already
                pass
            rgba.append(((r >> 3) << 11) | ((gg >> 3) << 6) | ((b >> 3) << 1) | (1 if a > 0 else 0))
            al.append(int(a))
    f.write(carr('logo_rgba', 'uint16_t', rgba, 16, '0x{:04X}'))
    f.write(carr('logo_alpha', 'uint8_t', al, 32))

print('meshes', len(meshes), 'tris total', sum(len(m[2]) for m in meshes))
print('wrote', OUT_C)
