#!/usr/bin/env python3
"""Twin Turbo characters: hand-pixeled 3/4 heads (ears, necks, That Franco's
quiff, beard and roman nose, tripleset's curls and square jaw) on a posed
skeleton with thin limbs, chunky fists and oversized sneakers. Each body part
is drawn with its own outline in z-order, so overlapping limbs stay readable.
32x48 frames, facing right."""
import math
import numpy as np
from PIL import Image

T, OUT, SKIN, SKIN_D, HAIR, HAIR_D, HAIR_L, SHIRT, SHIRT_D, SHIRT_L, ACC, PANTS, PANTS_D, SHOE, SHOE_D, EXTRA = range(16)

LOOKS = {
    'franco': dict(pal=['#000000', '#1a1026', '#f4b88e', '#c47c5a', '#3a2418', '#1e120c', '#6a4630',
                        '#2f8ae0', '#1c55a4', '#8ccaff', '#0c1838', '#8cb4e4', '#5a7cb4', '#f6f6f8', '#9aa0b8', '#7a3826'],
                   pattern='diamonds'),
    'tripleset': dict(pal=['#000000', '#1a1026', '#f6c4a0', '#cc8a68', '#7c4a2a', '#4a2a18', '#a8703e',
                           '#8a2436', '#561426', '#c04a58', '#f08a2c', '#4a4860', '#2c2a3a', '#f6f6f8', '#9aa0b8', '#94503a'],
                      pattern='stripes'),
}

# ---------------------------------------------------------------- heads (facing right)
# o outline  h hair  H hair dark  g hair light  s skin  S skin shade
# w eye white  e pupil  B/b beard  m mouth  t teeth/white  l dark skin line (nostril, lips, ear)
# C/c shirt collar (drawn over the neck)
HEAD_MAP = {'o': OUT, 'h': HAIR, 'H': HAIR_D, 'g': HAIR_L, 's': SKIN, 'S': SKIN_D,
            'w': SHOE, 'e': OUT, 'b': HAIR_L, 'B': HAIR, 'm': SKIN_D, 't': SHOE, 'l': EXTRA,
            'C': SHIRT, 'c': SHIRT_D}

FRANCO = [
    "............oooo.....",
    ".........oooggggo....",
    ".......oohhhhgggho...",
    ".....ooHhhHhhhhhggo..",
    "...ooHhhHhhhhhhhhggo.",
    "..oHhhHhhhhhHhhhhhgo.",
    ".oHHhHhhhhHhhhhhhHHo.",
    "oHHHhHhhhhHhhSSssso..",
    "oHHHhhHhhHSsssssso...",
    ".oHHHHhHhshhhHsshhso.",
    ".oHHHooHhsowwessweso.",
    "..oHosSohssSwesSsesso",
    "..oHoslohssSSssSsssso",
    "..oHoSsoBbsssssSsssso",
    "...oHooBbbbsssSllSSo.",
    ".....oSBbbbboBBBBBo..",
    "......oBbbbbbotttto..",
    "......oBbbbbbbolloo..",
    "......ooBbbbbbbbbo...",
    "......oloBbbbbbbo....",
    "......occoBbbbbo.....",
    ".....ocCCCooooo......",
]

TRIPLESET = [
    ".......oo..ooo.......",
    ".....oogghoggghoo....",
    "...oohhhghohhgghhgo..",
    "..ohgghhhHhhhhHhhggo.",
    ".oHhhhgHhhhgghhHhhgo.",
    ".oHhhhhHhhghhhHhhhhgo",
    "oHHhgghHhhhhhHhgghhho",
    "oHHhhhHhhHhhSShhhhHo.",
    ".oHHhHhhHsSsssHhHSso.",
    ".oHHHhhHhsHHHHssHHso.",
    ".oHHHooHhsoooessoeso.",
    "..oHosSohsswwesSwesso",
    "..oHoslohSsssssSssso.",
    "..oHoSsoSSssssssSsso.",
    "...oHooSSSssssSllSo..",
    ".....oSSSSsssssssso..",
    "......oSSSsssslllso..",
    "......oSoSSsssSSsso..",
    "......oSloSsssssso...",
    ".....occccoSSSSSo....",
    "....ocCCCCCoooooo....",
    "....ocCCCCCCo........",
]
HEAD_NECK = {'franco': (10, 21), 'tripleset': (10, 21)}   # template pixel on the neck top


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


# Run cycle: 8 frames built from the four classic key poses per step
# (contact, down, push-off/passing, flight), then mirrored onto the other leg.
# Upright torso with a light ~13 degree lean, head carried level, elbows bent
# near 90 degrees and swinging opposite the legs, 1px body bob (lowest on the
# down pose, highest in flight).
RUN_FOOT = [   # near-leg ankle per phase: (dx from hip, y, toe angle)
    (5.0, 43.5, -0.25),   # 0 contact: heel strikes ahead of the hip
    (1.5, 43.5, 0.0),     # 1 down: foot planted under the body, knee bent
    (-3.5, 43.0, 0.35),   # 2 push: rolling off the toe behind
    (-7.5, 40.0, 0.55),   # 3 flight: trailing leg extended
    (-6.5, 36.5, 0.45),   # 4 recovery: heel kicks up behind
    (-2.0, 35.0, 0.2),    # 5 tuck: foot under the seat, knee swinging through
    (3.0, 36.5, -0.1),    # 6 knee drive: thigh high in front
    (5.5, 40.5, -0.25),   # 7 reach: shin extends to meet the ground
]
RUN_HAND = [   # near-arm hand per phase, relative to the neck (opposite the near leg)
    (-4.0, 7.0), (-3.0, 7.5), (-0.5, 8.0), (3.0, 6.5),
    (4.5, 5.0), (3.5, 6.0), (0.5, 8.0), (-3.0, 7.5),
]
RUN_BOB = [0, 1, 0, -1, 0, 1, 0, -1]          # +1 = 1px lower
RUN_N = len(RUN_FOOT)


def run_pose(i):
    b = RUN_BOB[i]
    hip = (15.0, 32.0 + b)
    neck = (17.0, 23.0 + b)
    feet = []
    for k in (i, (i + RUN_N // 2) % RUN_N):     # near leg, far leg half a cycle later
        dx, fy, ang = RUN_FOOT[k]
        feet.append(((hip[0] + dx, fy), ang))
    hands = []
    for k in (i, (i + RUN_N // 2) % RUN_N):
        dx, dy = RUN_HAND[k]
        hands.append((neck[0] + dx, neck[1] + dy))
    return dict(hip=hip, neck=neck, feet=feet, hands=hands, head=(0, 0), kbend=1, elbow=-1)


def static_pose(kind):
    if kind in ('idle0', 'idle1'):            # relaxed ready stance; idle1 is the breath out
        b = 1 if kind == 'idle1' else 0
        return dict(hip=(15.5, 32.0), neck=(16.5, 23.0 + b), feet=[((19.0, 43.5), 0.0), ((12.5, 43.5), 0.0)],
                    hands=[(19.5, 32.0 + b), (13.0, 32.0 + b)], head=(0, 0), kbend=1, elbow=-1)
    if kind == 'jump':                         # take-off: near knee drives up, far leg trails the push
        return dict(hip=(15.0, 30.0), neck=(16.5, 21.0), feet=[((19.0, 36.5), -0.2), ((11.5, 41.5), 0.6)],
                    hands=[(10.5, 26.5), (21.5, 26.0)], head=(0, 0), kbend=1, elbow=-1)
    if kind == 'apex':                         # top of the arc: knees tucked, arms out for balance
        return dict(hip=(15.0, 30.5), neck=(16.5, 21.5), feet=[((18.5, 37.5), 0.0), ((13.0, 38.5), 0.3)],
                    hands=[(22.0, 27.0), (9.5, 27.5)], head=(0, 0), kbend=1, elbow=-1)
    if kind == 'fall':                         # dropping: legs reach for the ground, arms lift
        return dict(hip=(15.0, 30.0), neck=(16.5, 21.0), feet=[((18.5, 42.0), -0.1), ((12.5, 40.5), 0.3)],
                    hands=[(22.5, 25.0), (9.0, 24.5)], head=(0, 0), kbend=1, elbow=-1)
    if kind == 'land':                         # squash on touchdown
        return dict(hip=(15.0, 34.5), neck=(17.0, 25.5), feet=[((19.5, 43.5), 0.0), ((11.5, 43.5), 0.0)],
                    hands=[(21.5, 33.0), (12.5, 33.5)], head=(0, 0), kbend=1, elbow=-1)
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
        return dict(hip=(14.0, 40.5), neck=(9.0, 34.0), feet=[((27.0, 43.5), -0.1), ((24.0, 43.0), -0.1)],
                    hands=[(3.5, 43.0), (19.0, 38.5)], head=(0, 0), head_at=(0, 20), head_last=True, kbend=-1)
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
    if 'head_at' in P:
        hx, hy = P['head_at']
    else:
        hx = int(round(neck[0])) - HEAD_NECK[who][0] + P['head'][0]
        hy = int(round(neck[1])) - HEAD_NECK[who][1] + P['head'][1]
    if P.get('face') == 'hurt':
        tpl = hurt_face(tpl)

    def near_arm():
        ne = ik(shoulder_near, nhand, UARM, FARM, eb)
        limb(cv, shoulder_near, ne, nhand, 1.05, SKIN, SKIN_D)
        # short sleeve cap over the shoulder
        m, _, _ = ellipse_mask((shoulder_near[0] + 0.3, shoulder_near[1] + 0.8), 2.2, 2.0)
        cv.part(m, SHIRT)
        fist(cv, nhand, False)

    if P.get('head_last'):          # low poses: the head sits in front of the shoulders
        near_arm()
        cv.template(tpl, hx, hy, HEAD_MAP)
    else:                           # near arm on top
        cv.template(tpl, hx, hy, HEAD_MAP)
        near_arm()
    return cv.c


def hurt_face(tpl):
    """squeezed-shut eyes: near eye becomes a > squint, far eye a short line"""
    out = [list(r.replace('w', 's').replace('e', 's').replace('t', 'o')) for r in tpl]
    for (x, y) in ((10, 10), (11, 10), (12, 11), (13, 11), (16, 11), (17, 11)):
        out[y][x] = 'o'
    return [''.join(r) for r in out]


ORDER = ['run%d' % i for i in range(RUN_N)] + ['idle0', 'idle1', 'jump', 'apex', 'fall', 'land',
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
