#!/usr/bin/env python3
"""Twin Turbo characters, v3 (Sonic Advance style): big hand-pixeled heads with
large eyes and swoopy hair, on a posed skeleton with thin limbs, chunky fists
and oversized sneakers. Each body part is drawn with its own outline in
z-order, so overlapping limbs stay readable. 32x48 frames, facing right."""
import math
import numpy as np
from PIL import Image

T, OUT, SKIN, SKIN_D, HAIR, HAIR_D, HAIR_L, SHIRT, SHIRT_D, SHIRT_L, ACC, PANTS, PANTS_D, SHOE, SHOE_D, EXTRA = range(16)

LOOKS = {
    'franco': dict(pal=['#000000', '#1a1026', '#f4b88e', '#c47c5a', '#3a2418', '#1e120c', '#6a4630',
                        '#2f8ae0', '#1c55a4', '#8ccaff', '#0c1838', '#8cb4e4', '#5a7cb4', '#f6f6f8', '#9aa0b8', '#ffd24a'],
                   pattern='diamonds'),
    'tripleset': dict(pal=['#000000', '#1a1026', '#f6c4a0', '#cc8a68', '#a86c3c', '#6e4222', '#d89c5c',
                           '#8a2436', '#561426', '#c04a58', '#f08a2c', '#2e2c38', '#18161e', '#f6f6f8', '#9aa0b8', '#ff5a8a'],
                      pattern='stripes'),
}

# ---------------------------------------------------------------- heads (facing right)
# o outline  h hair  H hair dark  g hair light  s skin  S skin shade
# w eye white  e pupil  b beard  m mouth  t teeth/white
HEAD_MAP = {'o': OUT, 'h': HAIR, 'H': HAIR_D, 'g': HAIR_L, 's': SKIN, 'S': SKIN_D,
            'w': SHOE, 'e': OUT, 'b': HAIR_D, 'm': SKIN_D, 't': SHOE}

FRANCO = [
    "..........oooo.......",
    ".......ooogghhoo.....",
    ".....oogghhhhhhhoo...",
    "...ooghhhhhhhhhhhho..",
    ".oohhhhhhhhhhhhhhhho.",
    "oHHhhhhhhhhhhhhhhhho.",
    ".ooHHhhhhhhhhhhhhhho.",
    "..oHHhhhhhhhhhhhhhho.",
    ".oHHHhhhhhhhhhhhhsso.",
    "oHHHHhhhhhsooosssooo.",
    ".oHHHhhhssowwwosSweo.",
    "..oHHhosSsoweeossooo.",
    "..oHHhosSsoweeoSsssso",
    "..oHHhoSsssooosSsssso",
    "...oHHhossssssSSsSSo.",
    "...oHHbbsssssbbbbbo..",
    "....oHbbbsssbmtttbo..",
    ".....oHbbbbbbbmmbbo..",
    "......obbbbbbbbbbbo..",
    ".......oobbbbbbbbbo..",
    ".........ooooooooo...",
]

TRIPLESET = [
    ".......oo.ooo........",
    ".....oogoogghoo......",
    "...oogghhgghhhhoo....",
    "..oghhHhhhhHhhhhho...",
    ".oghhHhgghHhggghhho..",
    "ohhhHhghhHhghhhghhho.",
    "ohhHhghhHhghhhHhgghho",
    "ohHhhhhhhhhhhhHhghhgo",
    "ohHhhhhhhhhhhhhsshhho",
    ".oHhhhhhhhsooosssooo.",
    "..oHhhhhssowwwosoweo.",
    "..oHhhosSsoweeoSoeeo.",
    "..oHhhosSsoweeoSsooo.",
    "..oHhhoSsssooosSSsso.",
    "...oHhhossssssssssso.",
    "....ooooSsssssssssso.",
    "........oSssssssmmso.",
    ".........oSsssssssso.",
    "..........oSSsssssso.",
    "...........ooSSSssso.",
    ".............oooooo..",
]
HEAD_NECK = (11, 20)    # template pixel that sits on the neck top


def hexc(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def palette(who):
    return [hexc(c) for c in LOOKS[who]['pal']]


# ---------------------------------------------------------------- rasterising helpers
H, W = 48, 32
YY, XX = np.mgrid[0:H, 0:W]
PX, PY = XX + 0.5, YY + 0.5


def seg_mask(a, b, r):
    ax, ay = a; bx, by = b
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy or 1e-6
    t = np.clip(((PX - ax) * dx + (PY - ay) * dy) / L2, 0, 1)
    d = np.hypot(PX - (ax + t * dx), PY - (ay + t * dy))
    return d <= r


def ellipse_mask(c, rx, ry, ang=0.0):
    ca, sa = math.cos(ang), math.sin(ang)
    lx = (PX - c[0]) * ca + (PY - c[1]) * sa
    ly = -(PX - c[0]) * sa + (PY - c[1]) * ca
    return (lx / rx) ** 2 + (ly / ry) ** 2 <= 1.0, lx, ly


def dilate(m):
    n = m.copy()
    n[1:, :] |= m[:-1, :]; n[:-1, :] |= m[1:, :]; n[:, 1:] |= m[:, :-1]; n[:, :-1] |= m[:, 1:]
    return n


class Canvas:
    def __init__(self):
        self.c = np.zeros((H, W), np.uint8)

    def part(self, mask, colors):
        """mask: bool array; colors: uint8 array or scalar for the fill"""
        ring = dilate(mask) & ~mask
        self.c[ring] = OUT
        if np.isscalar(colors):
            self.c[mask] = colors
        else:
            self.c[mask] = colors[mask]

    def template(self, rows, x0, y0, cmap):
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                v = cmap.get(ch)
                if v is not None and 0 <= y0 + y < H and 0 <= x0 + x < W:
                    self.c[y0 + y, x0 + x] = v


def ik(hip, foot, l1, l2, bend):
    """2-bone IK: knee/elbow position; bend=+1 bends toward +x"""
    dx, dy = foot[0] - hip[0], foot[1] - hip[1]
    d = math.hypot(dx, dy)
    d = min(d, l1 + l2 - 1e-3)
    a = math.atan2(dy, dx)
    c = (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d)
    k = math.acos(max(-1, min(1, c)))
    ang = a - bend * k
    return (hip[0] + l1 * math.cos(ang), hip[1] + l1 * math.sin(ang))


# ---------------------------------------------------------------- body parts
def limb(cv, a, b, c, r, fill, shade_col):
    m = seg_mask(a, b, r) | seg_mask(b, c, r)
    col = np.full((H, W), fill, np.uint8)
    # simple light from the upper right: left/bottom fringe in shade
    sh = m & ~np.roll(m, -1, axis=1)
    col[sh] = shade_col
    cv.part(m, col)


SNEAKER = [            # facing right: ankle collar, toe cap, side stripe, sole
    "..oooo.....",
    ".oWWWWooo..",
    ".oAAWWWWWo.",
    "oWWAAWWWWWo",
    "oGGGGGGGGGo",
    ".ooooooooo.",
]


def shoe(cv, p, ang, far):
    """chunky sneaker; ankle at p. ang > 0 tips the toe down, < 0 lifts it"""
    cmap = {'o': OUT, 'W': SHOE_D if far else SHOE, 'A': ACC, 'G': OUT if far else SHOE_D}
    tilt = 1 if ang > 0.15 else -1 if ang < -0.15 else 0
    x0, y0 = int(p[0] + 0.5) - 4, int(p[1] + 0.5) - 3
    for y, r in enumerate(SNEAKER):
        for x, ch in enumerate(r):
            v = cmap.get(ch)
            if v is None:
                continue
            yy = y0 + y + (tilt if x >= 6 else 0)
            if 0 <= yy < H and 0 <= x0 + x < W:
                cv.c[yy, x0 + x] = v
    if tilt:   # close the seam the shear opens between heel and toe halves
        for y in range(len(SNEAKER)):
            a_, b_ = SNEAKER[y][5], SNEAKER[y][6]
            yy = y0 + y + (tilt if tilt > 0 else 0)
            if a_ != '.' and b_ != '.' and 0 <= yy < H and 0 <= x0 + 5 < W:
                v = cmap.get(SNEAKER[y - tilt][5] if 0 <= y - tilt < len(SNEAKER) else 'o', OUT)
                if cv.c[yy, x0 + 5] == 0:
                    cv.c[yy, x0 + 5] = v


def fist(cv, p, far):
    m, lx, ly = ellipse_mask(p, 2.1, 2.1)
    col = np.full((H, W), SKIN_D if far else SKIN, np.uint8)
    col[(ly < -0.6) & (lx > -0.4)] = SKIN if not far else SKIN_D
    col[(ly > 0.7) | (lx < -1.2)] = SKIN_D
    cv.part(m, col)


def torso(cv, who, neck, hip):
    m = seg_mask(neck, hip, 4.2)
    m &= (PY >= neck[1] - 1.5)
    col = np.full((H, W), SHIRT, np.uint8)
    # shading: back half dark, front edge light
    dx, dy = hip[0] - neck[0], hip[1] - neck[1]
    L = math.hypot(dx, dy)
    nx, ny = -dy / L, dx / L                  # normal pointing to -x side (back)
    side = (PX - neck[0]) * nx + (PY - neck[1]) * ny
    col[side > 1.6] = SHIRT_D
    col[side < -2.6] = SHIRT_L
    pat = LOOKS[who]['pattern']
    if pat == 'stripes':
        # orange pinstripes running down the jersey, following the lean
        along = (PX - neck[0]) * (-ny) + (PY - neck[1]) * nx
        s = np.floor(side + 100).astype(int) % 4
        col[m & (s == 1) & (col != SHIRT_L)] = ACC
    else:
        # diamond chain down the front-side panel
        d = (np.abs(side + 0.5) + np.abs(((PY - neck[1]) % 4) - 2)) <= 1.2
        col[d & (side > -2.2) & (side < 1.2)] = SHIRT_L
    # shorts
    t = ((PX - neck[0]) * dx + (PY - neck[1]) * dy) / (L * L)
    col[t > 0.78] = PANTS
    col[(t > 0.78) & (side > 1.2)] = PANTS_D
    # collar
    col[(PY < neck[1] + 1.0) & (side > -1.5) & (side < 2.5)] = SHIRT_D
    cv.part(m, col)


# ---------------------------------------------------------------- poses
THIGH, SHIN = 6.0, 6.2
UARM, FARM = 4.4, 4.2


def run_pose(i):
    ph = i / 6 * 2 * math.pi
    bob = -1.0 if i % 3 == 1 else 0.0
    hip = (13.5, 31.5 + bob)
    neck = (18.0, 23.0 + bob)
    legs = []
    for k, p in enumerate((ph, ph + math.pi)):
        fx = hip[0] + 9.0 * math.sin(p)
        lift = 6.0 * max(0.0, math.cos(p))
        fy = 43.5 - lift
        ang = 0.35 * math.cos(p) - 0.15 if lift > 0.5 else -0.05
        legs.append(((fx, fy), ang))
    arms = []
    for p in (ph + math.pi, ph):              # arm swings opposite its leg
        s = math.sin(p)
        hand = (neck[0] - 0.5 + 7.5 * s, neck[1] + 7.0 - 3.0 * max(0.0, s) + 1.0 * min(0.0, s))
        arms.append(hand)
    return dict(hip=hip, neck=neck, feet=legs, hands=arms, head=(0, 0), kbend=1)


def static_pose(kind):
    if kind in ('idle0', 'idle1'):
        b = 1 if kind == 'idle1' else 0
        return dict(hip=(15.0, 31.5), neck=(16.5, 22.5 + b * 0.5), feet=[((19.5, 43.5), 0.0), ((12.0, 43.5), 0.0)],
                    hands=[(20.0, 31.0 + b), (12.5, 31.0 + b)], head=(0, b), kbend=1, elbow=-1)
    if kind == 'jump':
        return dict(hip=(15.0, 30.0), neck=(17.0, 21.5), feet=[((22.0, 38.5), 0.3), ((10.0, 41.0), 0.6)],
                    hands=[(11.5, 31.0), (8.0, 28.0)], head=(0, 0), kbend=1, elbow=1)
    if kind == 'fall':
        return dict(hip=(15.0, 30.0), neck=(16.5, 21.0), feet=[((20.0, 42.0), 0.1), ((11.0, 41.0), -0.2)],
                    hands=[(25.5, 25.0), (7.0, 24.0)], head=(0, 0), kbend=1)
    if kind == 'hurt':
        return dict(hip=(15.0, 31.0), neck=(12.5, 23.0), feet=[((22.0, 40.0), 0.4), ((18.0, 43.5), 0.0)],
                    hands=[(23.5, 28.0), (5.0, 25.0)], head=(-4, 1), kbend=1, face='hurt')
    if kind == 'cheer0':
        return dict(hip=(15.0, 31.5), neck=(16.5, 22.5), feet=[((19.5, 43.5), 0.0), ((12.0, 43.5), 0.0)],
                    hands=[(23.5, 26.0), (10.5, 28.0)], head=(0, 0), kbend=1, elbow=1)
    if kind == 'cheer1':
        return dict(hip=(15.0, 30.0), neck=(16.5, 21.5), feet=[((19.5, 41.0), 0.3), ((12.0, 42.0), -0.2)],
                    hands=[(27.0, 29.0), (5.0, 27.0)], head=(0, 0), kbend=1)
    if kind == 'slide':
        return dict(hip=(12.0, 40.0), neck=(10.5, 31.5), feet=[((27.0, 43.5), -0.1), ((24.0, 43.0), -0.1)],
                    hands=[(5.0, 41.5), (16.0, 37.0)], head=(-6, 9), kbend=-1)
    raise KeyError(kind)


def pose(frame):
    if frame.startswith('run'):
        return run_pose(int(frame[3:]))
    return static_pose(frame)


def compose(who, frame):
    P = pose(frame)
    cv = Canvas()
    hip, neck = P['hip'], P['neck']
    shoulder_far = (neck[0] - 0.5, neck[1] + 2.2)
    shoulder_near = (neck[0] - 0.5, neck[1] + 2.6)
    (ffoot, fang), (nfoot, nang) = P['feet'][1], P['feet'][0]
    fhand, nhand = P['hands'][1], P['hands'][0]
    eb = P.get('elbow', -1)
    kb = P['kbend']
    # far arm, far leg (shaded)
    fe = ik(shoulder_far, fhand, UARM, FARM, eb)
    limb(cv, shoulder_far, fe, fhand, 1.05, SKIN_D, OUT)
    hipf = (hip[0] - 1.0, hip[1])
    fk = ik(hipf, (ffoot[0] - 1.5, ffoot[1] - 1.0), THIGH, SHIN, kb)
    limb(cv, hipf, fk, (ffoot[0] - 1.5, ffoot[1] - 1.0), 1.3, PANTS_D, OUT)
    shoe(cv, ffoot, fang, True)
    fist(cv, fhand, True)
    # torso and near leg
    torso(cv, who, neck, hip)
    hipn = (hip[0] + 0.8, hip[1])
    nk = ik(hipn, (nfoot[0] - 1.5, nfoot[1] - 1.0), THIGH, SHIN, kb)
    limb(cv, hipn, nk, (nfoot[0] - 1.5, nfoot[1] - 1.0), 1.3, PANTS, PANTS_D)
    shoe(cv, nfoot, nang, False)
    # head
    tpl = FRANCO if who == 'franco' else TRIPLESET
    hx = int(round(neck[0])) - HEAD_NECK[0] + P['head'][0]
    hy = int(round(neck[1])) - HEAD_NECK[1] + P['head'][1]
    if P.get('face') == 'hurt':
        tpl = hurt_face(tpl)
    cv.template(tpl, hx, hy, HEAD_MAP)
    # near arm on top
    ne = ik(shoulder_near, nhand, UARM, FARM, eb)
    limb(cv, shoulder_near, ne, nhand, 1.05, SKIN, SKIN_D)
    # short sleeve cap over the shoulder
    m, _, _ = ellipse_mask((shoulder_near[0] + 0.3, shoulder_near[1] + 0.8), 2.2, 2.0)
    cv.part(m, SHIRT)
    fist(cv, nhand, False)
    return cv.c


def hurt_face(tpl):
    """squeezed-shut eyes: replace eye whites/pupils with a > shape"""
    out = []
    for r in tpl:
        out.append(r.replace('w', 's').replace('e', 's'))
    out = [list(r) for r in out]
    # draw >< style squint at the near eye (cols 12-15, rows 10-12)
    for (x, y) in ((12, 10), (13, 11), (14, 11), (12, 12)):
        out[y][x] = 'o'
    return [''.join(r) for r in out]


ORDER = ['run%d' % i for i in range(6)] + ['idle0', 'idle1', 'jump', 'fall',
                                           'hurt', 'slide', 'cheer0', 'cheer1']


def preview(path, frames, scale=4):
    rows = []
    for who in ('franco', 'tripleset'):
        pal = palette(who)
        strip = np.concatenate([compose(who, f) for f in frames], axis=1)
        rgb = np.zeros(strip.shape + (3,), np.uint8)
        rgb[:] = (240, 170, 200)
        for i in range(1, 16):
            rgb[strip == i] = pal[i]
        rows.append(rgb)
    img = np.concatenate(rows, axis=0)
    Image.fromarray(img).resize((img.shape[1] * scale, img.shape[0] * scale), Image.NEAREST).save(path)


if __name__ == '__main__':
    import sys
    for t in (FRANCO, TRIPLESET):
        assert all(len(r) == len(t[0]) for r in t), [len(r) for r in t]
    preview(sys.argv[1], sys.argv[2:] or ORDER, 4)
