/* Physics engine. Compiled as ARM code and placed in IWRAM (fast 32-bit bus). */
#include "physics.h"
#include "gen/pieces_gen.h"
_Static_assert(PIECE_MAX_VERTS <= MAX_VERTS, "piece parts have more vertices than physics.h allows");
#include "gen/tables_gen.h"

#ifdef HOST
#define ROM_CODE
#else
/* cold code: Thumb in ROM (ARM code would fetch 32 bits over the 16-bit cart bus) */
#define ROM_CODE __attribute__((section(".romcode"), noinline, target("thumb")))
#endif

/* ---- tuning ----------------------------------------------------------
 * Collide once per frame, then SUBSTEPS x (gravity, warm start, solve with a
 * Baumgarte push-out, integrate positions, relax without the push) -- the
 * substepping of Box2D v3 with rigid (non-spring) contacts and a 2-point block
 * solver. Small blocks under real gravity need the substeps.
 *
 * Settled blocks sleep and then count as solid ground for anything that lands on
 * them (see phys_step section 4), with a static balance check that wakes a
 * tower when its load no longer sits over its support. That keeps stacks steady
 * and the per-frame cost down to the handful of blocks actually moving. */
#define DT        (FX_ONE / 60)
#ifndef SUBSTEPS
#define SUBSTEPS  4
#endif
#ifndef ITERS
#if defined(HOST) && defined(HOST_ITERS)
int phys_iters = 1;
#define ITERS phys_iters
#else
#define ITERS 1
#endif
#endif
#define H24       ((1 << 24) / (60 * SUBSTEPS))   /* substep in Q24 seconds */
#define H         (FX_ONE / (60 * SUBSTEPS))
#define INV_H     ((60 * SUBSTEPS) * FX_ONE)
#define GRAVITY   FX(392.4)        /* 9.81 m/s^2 in cells/s^2 (1 cell = 2.5 cm) */
#define MAX_BIAS_VEL FX(60.0)
#define MAX_VEL   FX(300.0)        /* cells/s; a block dropped from 100 cells up lands at ~280 */
#define MAX_ANG_VEL FX(40.0)       /* rad/s */
#ifndef BAUMGARTE
#define BAUMGARTE 0.2
#endif
#define BAUMGARTE_RATE FX(BAUMGARTE * 60 * SUBSTEPS)
#define LINEAR_SLOP FX(0.02)
#define MARGIN    FX(0.04)         /* speculative contact distance */
#define REST      FX(0.25)         /* restitution (Vision Pro classic) */
#define REST_VEL  FX(12.0)         /* only bounce above this approach speed */
#define FRICTION  FX(0.8)
#define FRICTION_BASE FX(0.9)
#define SLEEP_LIN2 FX(0.6 * 0.6)
#define SLEEP_ANG  FX(0.12)
#ifndef SLEEP_TIME
#define SLEEP_TIME 30
#endif
#define MAX_ARB   512          /* contact manifolds kept between steps */
#define SUPPORT_NY FX(0.3)       /* contact normal steeper than this = resting on */
#define IMPACT_WAKE FX(10.0)     /* approach speed that wakes a sleeping body */
#define CULL_VSLACK FX(20.0)     /* speed a step may add to an approach (gravity alone: 6.5) */
#define TIP_MARGIN FX(0.0)
#ifndef MAX_PTS
#define MAX_PTS   512          /* solver points per step (the first PTS_IW in IWRAM) */
#endif
#define PTS_IW    (MAX_PTS < 96 ? MAX_PTS : 96)

Body phys_bodies[MAX_BODIES] EWRAM_BSS;
BodyVel phys_vel[MAX_BODIES];
int phys_count, phys_awake, phys_points;
u32 phys_prof[8];
#ifdef PROF_FINE   /* bench builds: cycle counts per section from timer 3 */
extern u32 prof_fine[16];
#define PF0(v) u16 v = REG_TM3CNT_L
#define PF1(k, v) prof_fine[k] += (u16)(REG_TM3CNT_L - v)
#define PFN(k) prof_fine[k]++
#else
#define PF0(v)
#define PF1(k, v)
#define PFN(k)
#endif
#ifdef HOST
#define PROF(k)
#else
#define PROF(k) phys_prof[k] = REG_TM2CNT_L
#endif

/* Persistent contact manifolds ("arbiters"), EWRAM. Those between two resting
   bodies stay where they are, step after step, at the front of arbs[]; each step
   appends the ones it computes for moving bodies. Last step's arbiters of moving
   bodies are set aside in prev_aw[] for warm starting. */
typedef struct { u8 a, b, pa, pb; u8 n; s8 up; u16 feat[2]; u8 tm; fx Pn[2], Pt[2]; fx cx[2]; s16 nx, ny; } Arbiter;   /* up: +1 b rests on a, -1 a rests on b; cx, nx/ny (Q14): contact x and normal, for wake_loads; tm: points actually touching, for tip_check */
static Arbiter arbs[MAX_ARB] EWRAM_BSS, prev_aw[MAX_ARB] EWRAM_BSS;
static int narb, nprev, ncarried;   /* narb: arbs[] in use; [0, ncarried) were carried over this step */
static u8 arb_rescan;               /* a body woke or went away: look through all of arbs[] */
#define ARB_HASH 1024
static u16 arb_hash[ARB_HASH] EWRAM_BSS;

/* per-step solver data: one SMan per manifold (contact normal, the two bodies,
   block solver), one SPt per contact point. The first PTS_IW points (and their
   manifolds) live in fast IWRAM; a big pile-up spills into EWRAM, slower but
   never dropping contacts, which would let blocks sink into each other and
   jitter forever. */
typedef struct {
  u8 a, b, n, blk;                   /* bodies, points (1-2), use the 2-point block solver */
  u8 dyn;                            /* bit 0: a moves, bit 1: b moves (else it is ground) */
  u8 pad; u16 arb;                   /* arbiter index */
  fx nx, ny, fric;
  fx anx, any, bnx, bny;             /* normal scaled by each body's inverse mass (0 for ground) */
  fx k11, k12, k22, i11, i12, i22;   /* block solver */
} SMan;
typedef struct {
  fx rn1, rn2, rt1, rt2, kna, knb, kta, ktb;
  fx mn, mt, Pn, Pt, sep0, vn0, maxPn;
} SPt;
#define MAX_MAN (MAX_PTS * 3 / 4)
#define MAN_IW  (PTS_IW * 3 / 4)
static SMan mans[MAN_IW];
static SPt pts[PTS_IW];
static SMan mans_x[MAX_MAN - MAN_IW + 1] EWRAM_BSS;
static SPt pts_x[MAX_PTS - PTS_IW + 1] EWRAM_BSS;
static int nman, npts;            /* totals this step */
static int nman_iw, npts_iw;      /* how many of them are in IWRAM (a prefix) */
static fx body_invM[MAX_BODIES] EWRAM_BSS, body_invI[MAX_BODIES] EWRAM_BSS;

static u8 uf_parent[MAX_BODIES] EWRAM_BSS;
static u8 wake_req[MAX_BODIES] EWRAM_BSS, wr_list[MAX_BODIES] EWRAM_BSS;   /* wake requests (phys_wake, impacts) */
static int wr_n;
static void request_wake(int i) { if (!wake_req[i]) { wake_req[i] = 1; wr_list[wr_n++] = i; } }
static u8 tip_woke[MAX_BODIES] EWRAM_BSS, tip_ok[MAX_BODIES] EWRAM_BSS;   /* see tip_note_sleep */

/* ---- math ------------------------------------------------------------ */
fx fx_sin(fx a) {
  s32 t = (s32)(((s64)a * 10680707) >> 16);   /* a * 1024/(2pi), Q16 */
  int i = (t >> 16) & 1023; s32 f = t & 0xFFFF;
  s32 s0 = sin_lut[i], s1 = sin_lut[(i + 1) & 1023];
  return s0 + (s32)(((s64)(s1 - s0) * f) >> 16);
}
fx fx_cos(fx a) { return fx_sin(a + FX_PI / 2); }

/* unsigned divide in ARM code from IWRAM: libgcc's runs as Thumb from ROM and
   costs several times more */
static u32 udiv32(u32 n, u32 d) {
  u32 q = 0, bit = 1;
  while (d < n && !(d & 0x80000000u)) { d <<= 1; bit <<= 1; }
  do { if (n >= d) { n -= d; q |= bit; } d >>= 1; bit >>= 1; } while (bit);
  return q;
}

/* (num << 16) / den for 0 <= num < den < 2^15: a 16-bit quotient, one
   compare-and-subtract per bit */
static inline u32 frac16(u32 num, u32 den) {
  u32 q = 0;
  if (num >= den) return 0xFFFF;
#pragma GCC unroll 16
  for (int k = 0; k < 16; k++) { num <<= 1; q <<= 1; if (num >= den) { num -= den; q |= 1; } }
  return q;
}

static inline u32 recip(fx k) {        /* 1/k in Q16, k > 0 (Q16) */
  if (k <= 2) return 0x7FFFFFFF;
  return udiv32(0xFFFFFFFFu, (u32)k);
}

/* ---- geometry -------------------------------------------------------- */
static void transform_parts(int shape, fx px, fx py, fx c, fx s, Poly* out, V2* bmin, V2* bmax) {
  const PieceDef* d = &piece_defs[shape];
  fx minx = 0x7FFFFFFF, miny = 0x7FFFFFFF, maxx = -0x7FFFFFFF, maxy = -0x7FFFFFFF;
  for (int p = 0; p < d->nparts; p++) {
    const PiecePart* pp = &d->parts[p];
    Poly* w = &out[p];
    w->n = pp->n;
    fx pminx = 0x7FFFFFFF, pminy = 0x7FFFFFFF, pmaxx = -0x7FFFFFFF, pmaxy = -0x7FFFFFFF;
    for (int i = 0; i < pp->n; i++) {
      fx lx = pp->v[i][0], ly = pp->v[i][1];
      fx x = px + (fx)(((s64)c * lx - (s64)s * ly + 0x8000) >> 16);
      fx y = py + (fx)(((s64)s * lx + (s64)c * ly + 0x8000) >> 16);
      w->v[i].x = x; w->v[i].y = y;
      fx nx = pp->nrm[i][0], ny = pp->nrm[i][1];
      w->nrm[i].x = (fx)(((s64)c * nx - (s64)s * ny + 0x8000) >> 16);
      w->nrm[i].y = (fx)(((s64)s * nx + (s64)c * ny + 0x8000) >> 16);
      if (x < pminx) pminx = x; if (x > pmaxx) pmaxx = x;
      if (y < pminy) pminy = y; if (y > pmaxy) pmaxy = y;
    }
    w->bmin.x = pminx; w->bmin.y = pminy; w->bmax.x = pmaxx; w->bmax.y = pmaxy;
    if (pminx < minx) minx = pminx; if (pmaxx > maxx) maxx = pmaxx;
    if (pminy < miny) miny = pminy; if (pmaxy > maxy) maxy = pmaxy;
  }
  bmin->x = minx; bmin->y = miny; bmax->x = maxx; bmax->y = maxy;
}

ROM_CODE int phys_piece_world(int shape, fx x, fx y, fx angle, Poly* polys) {
  V2 a, b;
  transform_parts(shape, x, y, fx_cos(angle), fx_sin(angle), polys, &a, &b);
  return piece_defs[shape].nparts;
}

static void update_body(Body* b) {
  b->c = fx_cos(b->a); b->s = fx_sin(b->a);
  transform_parts(b->shape, b->p.x, b->p.y, b->c, b->s, b->wp, &b->bmin, &b->bmax);
}

/* max over edges of A of min separation of B's verts along that edge normal */
static fx sep_margin = MARGIN;
/* In 32 bits: normals and coordinates (relative to A's first vertex) in Q12;
   whatever is tested here lies within a few cells of the other polygon, so the
   products fit easily. The axis choice and the early out need no more; contact
   depths are computed exactly afterwards. B's vertices are projected with one
   multiply-add each per edge (a triangle repeats its last vertex). Q24 inside,
   Q16 out. */
static fx max_separation(const Poly* A, const Poly* B, int* edge) {
  s32 best = -0x7FFFFFFF, lim = sep_margin << 8; int bi = 0, nb = B->n;
  fx ox = A->v[0].x, oy = A->v[0].y;
  s32 bx[4], by[4];
  for (int j = 0; j < 4; j++) { int q = j < nb ? j : nb - 1; bx[j] = (B->v[q].x - ox) >> 4; by[j] = (B->v[q].y - oy) >> 4; }
  for (int i = 0; i < A->n; i++) {
    s32 nx = A->nrm[i].x >> 4, ny = A->nrm[i].y >> 4;
    s32 si = nx * bx[0] + ny * by[0], d;
    d = nx * bx[1] + ny * by[1]; if (d < si) si = d;
    d = nx * bx[2] + ny * by[2]; if (d < si) si = d;
    d = nx * bx[3] + ny * by[3]; if (d < si) si = d;
    si -= nx * ((A->v[i].x - ox) >> 4) + ny * ((A->v[i].y - oy) >> 4);
    if (si > best) { best = si; bi = i; if (si > lim) break; }   /* separating axis: done */
  }
  *edge = bi;
  return best >> 8;
}

typedef struct { V2 p; fx sep; u16 feat; } CPoint;
typedef struct { V2 n; int count; CPoint c[2]; } Manifold;

static int clip(V2* vin, u8* fin, V2* vout, u8* fout, V2 n, fx off, u8 clipEdge) {
  int k = 0;
  fx d0 = fxdot(n, vin[0]) - off, d1 = fxdot(n, vin[1]) - off;
  if (d0 <= 0) { vout[k] = vin[0]; fout[k++] = fin[0]; }
  if (d1 <= 0) { vout[k] = vin[1]; fout[k++] = fin[1]; }
  if ((d0 ^ d1) < 0 && k < 2) {
    s32 num = d0, den = d0 - d1;
    if (den < 0) { num = -num; den = -den; }
    while (den > 0x7FFF) { den >>= 1; num >>= 1; }
    fx t = !den ? 0 : num < 0 ? -(fx)frac16(-num, den) : (fx)frac16(num, den);
    vout[k].x = vin[0].x + fxmul(t, vin[1].x - vin[0].x);
    vout[k].y = vin[0].y + fxmul(t, vin[1].y - vin[0].y);
    fout[k++] = clipEdge | 0x80;
  }
  return k;
}

/* polygon vs polygon; normal points from A to B */
static int collide(const Poly* A, const Poly* B, Manifold* m) {
  int ea, eb;
  fx sa = max_separation(A, B, &ea);
  if (sa > sep_margin) return 0;
  fx sb = max_separation(B, A, &eb);
  if (sb > sep_margin) return 0;
  const Poly *P1, *P2; int e1, flip;
  if (sb > sa + FX(0.005)) { P1 = B; P2 = A; e1 = eb; flip = 1; }
  else { P1 = A; P2 = B; e1 = ea; flip = 0; }
  V2 n1 = P1->nrm[e1];
  /* incident edge: most anti-parallel normal on P2 */
  int ie = 0; fx mind = 0x7FFFFFFF;
  for (int i = 0; i < P2->n; i++) {
    fx d = fxdot(n1, P2->nrm[i]);
    if (d < mind) { mind = d; ie = i; }
  }
  V2 inc[2]; u8 incf[2];
  inc[0] = P2->v[ie]; int ie2 = ie + 1 == P2->n ? 0 : ie + 1;
  inc[1] = P2->v[ie2];
  incf[0] = ie; incf[1] = ie2;
  V2 v11 = P1->v[e1], v12 = P1->v[(e1 + 1) % P1->n];
  V2 t = { -n1.y, n1.x };   /* tangent along edge (CCW) */
  V2 nt = { -t.x, -t.y };
  V2 c1[2], c2[2]; u8 f1[2], f2[2];
  if (clip(inc, incf, c1, f1, nt, -fxdot(t, v11), 0x10) < 2) return 0;
  if (clip(c1, f1, c2, f2, t, fxdot(t, v12), 0x20) < 2) return 0;
  fx front = fxdot(n1, v11);
  int k = 0;
  for (int i = 0; i < 2; i++) {
    fx sep = fxdot(n1, c2[i]) - front;
    if (sep <= sep_margin) {
      m->c[k].p = c2[i]; m->c[k].sep = sep;
      m->c[k].feat = (u16)(e1 | (f2[i] << 3) | (flip << 15));
      k++;
    }
  }
  m->n = flip ? (V2){ -n1.x, -n1.y } : n1;
  m->count = k;
  /* keep a stable order (along the world tangent) so warm starting can match by index */
  if (k == 2) {
    V2 wt = { -m->n.y, m->n.x };
    if (fxdot(wt, m->c[0].p) > fxdot(wt, m->c[1].p)) { CPoint tmp = m->c[0]; m->c[0] = m->c[1]; m->c[1] = tmp; }
  }
  return k;
}

/* the bodies live in EWRAM (slow 16-bit bus); the SAT reads every vertex
   several times, so work on IWRAM (stack) copies */
static int collide_fast(const Poly* A, const Poly* B, Manifold* m) {
  Poly a, b;
  int na = A->n, nb = B->n;
  a.n = na; b.n = nb;
  for (int i = 0; i < na; i++) { a.v[i] = A->v[i]; a.nrm[i] = A->nrm[i]; }
  for (int i = 0; i < nb; i++) { b.v[i] = B->v[i]; b.nrm[i] = B->nrm[i]; }
  return collide(&a, &b, m);
}

static inline int aabb_overlap(V2 amin, V2 amax, V2 bmin, V2 bmax, fx m) {
  return !(amax.x + m < bmin.x || bmax.x + m < amin.x || amax.y + m < bmin.y || bmax.y + m < amin.y);
}

/* ---- world management ------------------------------------------------ */
/* broad phase state (see broadphase()) */
static u8 bp_order[MAX_BODIES] EWRAM_BSS, bp_in[MAX_BODIES] EWRAM_BSS;
static int bp_n;
static u8 ph;                      /* step phase, PH_IDLE between steps */
static u8 bp_dirty;                /* bodies were added or removed */
ROM_CODE static void body_reset_scratch(int i);
ROM_CODE void phys_init(void) {
  for (int i = 0; i < MAX_BODIES; i++) { phys_bodies[i].flags = 0; bp_in[i] = 0; wake_req[i] = 0; }
  bp_n = 0; ph = 0; wr_n = 0; bp_dirty = 1;
  phys_count = 0; narb = nprev = ncarried = 0; arb_rescan = 0; npts = 0; nman = 0; nman_iw = npts_iw = 0;
}

static int alloc_body(void) {
  for (int i = 0; i < MAX_BODIES; i++) if (!(phys_bodies[i].flags & BODY_USED)) {
    if (i >= phys_count) phys_count = i + 1;
    return i;
  }
  return -1;
}

ROM_CODE int phys_add_static_box(fx cx, fx cy, fx hw, fx hh) {
  int i = alloc_body(); if (i < 0) return -1;
  bp_dirty = 1;
  Body* b = &phys_bodies[i];
  b->flags = BODY_USED | BODY_STATIC; b->shape = 0xFF; b->nparts = 1;
  b->p.x = cx; b->p.y = cy; b->a = 0; b->c = FX_ONE; b->s = 0;
  Poly* p = &b->wp[0]; p->n = 4;
  p->v[0] = (V2){ cx - hw, cy - hh }; p->v[1] = (V2){ cx + hw, cy - hh };
  p->v[2] = (V2){ cx + hw, cy + hh }; p->v[3] = (V2){ cx - hw, cy + hh };
  p->nrm[0] = (V2){ 0, -FX_ONE }; p->nrm[1] = (V2){ FX_ONE, 0 };
  p->nrm[2] = (V2){ 0, FX_ONE }; p->nrm[3] = (V2){ -FX_ONE, 0 };
  p->bmin = p->v[0]; p->bmax = p->v[2]; b->bmin = p->v[0]; b->bmax = p->v[2];
  BodyVel* v = &phys_vel[i]; v->vx = v->vy = v->w = 0; body_invM[i] = 0; body_invI[i] = 0;
  body_reset_scratch(i);
  return i;
}

ROM_CODE int phys_add_piece(int shape, int color, fx x, fx y, fx angle) {
  int i = alloc_body(); if (i < 0) return -1;
  bp_dirty = 1;
  Body* b = &phys_bodies[i];
  const PieceDef* d = &piece_defs[shape];
  b->flags = ph ? BODY_USED | BODY_NEW : BODY_USED; b->shape = shape; b->color = color; b->nparts = d->nparts;
  b->p.x = x; b->p.y = y; b->a = angle; b->sleepTime = 0; b->frx = b->fry = b->fra = 0;
  tip_woke[i] = tip_ok[i] = 0;
  update_body(b);
  BodyVel* v = &phys_vel[i];
  v->vx = v->vy = v->w = 0; v->dpx = v->dpy = v->dq = 0;
  body_invM[i] = recip(d->mass); body_invI[i] = recip(d->inertia);
  body_reset_scratch(i);
  return i;
}

ROM_CODE int phys_overlaps(const Poly* polys, int n, fx tol) {
  V2 mn = polys[0].bmin, mx = polys[0].bmax;
  for (int k = 1; k < n; k++) {
    if (polys[k].bmin.x < mn.x) mn.x = polys[k].bmin.x; if (polys[k].bmin.y < mn.y) mn.y = polys[k].bmin.y;
    if (polys[k].bmax.x > mx.x) mx.x = polys[k].bmax.x; if (polys[k].bmax.y > mx.y) mx.y = polys[k].bmax.y;
  }
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if (!(b->flags & BODY_USED)) continue;
    if (!aabb_overlap(mn, mx, b->bmin, b->bmax, 0)) continue;
    for (int k = 0; k < n; k++) for (int p = 0; p < b->nparts; p++) {
      if (!aabb_overlap(polys[k].bmin, polys[k].bmax, b->wp[p].bmin, b->wp[p].bmax, 0)) continue;
      int e;
      sep_margin = MARGIN;
      if (max_separation(&polys[k], &b->wp[p], &e) > -tol) continue;
      if (max_separation(&b->wp[p], &polys[k], &e) > -tol) continue;
      return 1;
    }
  }
  return 0;
}

/* ---- stepping -------------------------------------------------------- */
static inline unsigned arb_key_hash(int a, int b, int pa, int pb) {
  return ((a * 31 + b) * 7 + pa * 3 + pb * 5) & (ARB_HASH - 1);
}

ROM_CODE static void build_prev_hash(void) {
  u32* h32 = (u32*)arb_hash;
  for (int i = 0; i < ARB_HASH / 2; i++) h32[i] = 0xFFFFFFFFu;
  for (int i = 0; i < nprev; i++) {
    Arbiter* r = &prev_aw[i];
    unsigned h = arb_key_hash(r->a, r->b, r->pa, r->pb);
    while (arb_hash[h] != 0xFFFF) h = (h + 1) & (ARB_HASH - 1);
    arb_hash[h] = i;
  }
}
static Arbiter* find_prev(int a, int b, int pa, int pb) {
  unsigned h = arb_key_hash(a, b, pa, pb);
  while (arb_hash[h] != 0xFFFF) {
    Arbiter* r = &prev_aw[arb_hash[h]];
    if (r->a == a && r->b == b && r->pa == pa && r->pb == pb) return r;
    h = (h + 1) & (ARB_HASH - 1);
  }
  return 0;
}

static int uf_find(int x) {
  while (uf_parent[x] != x) { uf_parent[x] = uf_parent[uf_parent[x]]; x = uf_parent[x]; }
  return x;
}
static void uf_union(int a, int b) { a = uf_find(a); b = uf_find(b); if (a != b) uf_parent[a] = b; }

static inline int is_awake_dyn(const Body* b) { return (b->flags & (BODY_USED | BODY_STATIC | BODY_SLEEP | BODY_NEW)) == BODY_USED; }

static inline fx inv_scale(fx x, u32 r, int rs) {
  s64 v = (s64)x * r;
  return (fx)(rs >= 0 ? v >> rs : v << -rs);
}

static SPt* add_points(int ai, int bi, int pa, int pb, const Manifold* m, Arbiter* na, Arbiter* old, Arbiter* out) {
  Body* A = &phys_bodies[ai]; Body* B = &phys_bodies[bi];
  BodyVel* va = &phys_vel[ai]; BodyVel* vb = &phys_vel[bi];
  /* a sleeping body is solid ground for whatever touches it */
  fx imA = (A->flags & BODY_SLEEP) ? 0 : body_invM[ai], iiA = (A->flags & BODY_SLEEP) ? 0 : body_invI[ai];
  fx imB = (B->flags & BODY_SLEEP) ? 0 : body_invM[bi], iiB = (B->flags & BODY_SLEEP) ? 0 : body_invI[bi];
  fx nx = m->n.x, ny = m->n.y;
  SMan* mn; SPt* s0;
  if (nman == nman_iw && nman < MAN_IW && npts + m->count <= PTS_IW) {
    mn = &mans[nman]; s0 = &pts[npts]; nman_iw = nman + 1; npts_iw = npts + m->count;
  } else { mn = &mans_x[nman - nman_iw]; s0 = &pts_x[npts - npts_iw]; }
  nman++; npts += m->count;
  mn->a = ai; mn->b = bi; mn->n = m->count; mn->blk = 0;
  mn->dyn = (imA ? 1 : 0) | (imB ? 2 : 0);
  mn->arb = na - out;
  mn->nx = nx; mn->ny = ny;
  mn->fric = ((A->flags | B->flags) & BODY_STATIC) ? FRICTION_BASE : FRICTION;
  mn->anx = fxmul(imA, nx); mn->any = fxmul(imA, ny); mn->bnx = fxmul(imB, nx); mn->bny = fxmul(imB, ny);
  na->tm = 0;
  for (int k = 0; k < m->count; k++) {
    SPt* s = s0 + k;
    fx r1x = m->c[k].p.x - A->p.x, r1y = m->c[k].p.y - A->p.y;
    fx r2x = m->c[k].p.x - B->p.x, r2y = m->c[k].p.y - B->p.y;
    /* cross products r x n and r x t, t = (-ny, nx) */
    s->rn1 = (fx)(((s64)r1x * ny - (s64)r1y * nx + 0x8000) >> 16);
    s->rn2 = (fx)(((s64)r2x * ny - (s64)r2y * nx + 0x8000) >> 16);
    s->rt1 = (fx)(((s64)r1x * nx + (s64)r1y * ny + 0x8000) >> 16);
    s->rt2 = (fx)(((s64)r2x * nx + (s64)r2y * ny + 0x8000) >> 16);
    s->kna = fxmul(iiA, s->rn1); s->knb = fxmul(iiB, s->rn2);
    s->kta = fxmul(iiA, s->rt1); s->ktb = fxmul(iiB, s->rt2);
    s->Pn = 0; s->Pt = 0;
    if (old) {
      if (old->n == m->count) { s->Pn = old->Pn[k]; s->Pt = old->Pt[k]; }
      else for (int j = 0; j < old->n; j++) if (old->feat[j] == m->c[k].feat) { s->Pn = old->Pn[j]; s->Pt = old->Pt[j]; }
    }
    na->feat[k] = m->c[k].feat; na->cx[k] = m->c[k].p.x;
    if (m->c[k].sep <= MARGIN) na->tm |= 1 << k;
    fx kn = imA + imB + fxmul(s->kna, s->rn1) + fxmul(s->knb, s->rn2);
    fx kt = imA + imB + fxmul(s->kta, s->rt1) + fxmul(s->ktb, s->rt2);
    s->mn = recip(kn); s->mt = recip(kt);
    s->sep0 = m->c[k].sep;
    s->vn0 = (fx)(((s64)(vb->vx - va->vx) * nx + (s64)(vb->vy - va->vy) * ny + (s64)vb->w * s->rn2 - (s64)va->w * s->rn1 + 0x8000) >> 16);
    s->maxPn = 0;
  }
  na->n = m->count; na->nx = nx >> 2; na->ny = ny >> 2;
  if (m->count == 2) {
    SPt* s1 = s0 + 1;
    fx im = imA + imB;
    fx k11 = im + fxmul(s0->kna, s0->rn1) + fxmul(s0->knb, s0->rn2);
    fx k22 = im + fxmul(s1->kna, s1->rn1) + fxmul(s1->knb, s1->rn2);
    fx k12 = im + fxmul(s0->kna, s1->rn1) + fxmul(s0->knb, s1->rn2);
    s64 det = (s64)k11 * k22 - (s64)k12 * k12;            /* Q32 */
    if (det > 0 && (s64)k11 * k11 < det * 500) {
      mn->blk = 1;
      mn->k11 = k11; mn->k12 = k12; mn->k22 = k22;
      /* inverse of K = adj(K) / det. One 32-bit reciprocal of the normalised det
         (17 significant bits) instead of three 64-bit divisions, which libgcc
         does in slow Thumb code from ROM */
      int sh = 0; u64 d = (u64)det;
      while (d >= (1ull << 31)) { d >>= 1; sh++; }
      while (d < (1u << 30)) { d <<= 1; sh--; }
      u32 r = udiv32(0xFFFFFFFFu, (u32)(d >> 15));        /* ~ 2^47 / d */
      int rs = 15 + sh;                              /* x * 2^32 / det = x * r >> rs */
      mn->i11 = inv_scale(k22, r, rs);
      mn->i12 = -inv_scale(k12, r, rs);
      mn->i22 = inv_scale(k11, r, rs);
    }
  }
  return s0;
}

/* apply impulse dP along (dx, dy) at a contact; a ground side (inverse mass 0)
   is skipped, so is a zero impulse (a clamped or idle contact) */
/* apply impulse dP along the normal (APPLY_N) or the tangent t = (-ny, nx)
   (APPLY_T): with the normal premultiplied by the inverse masses, three
   multiplies per moving body */
#define APPLY_N(m, va, vb, dP, ka, kb) do { fx dP_ = (dP); if (dP_) { \
    if ((m)->dyn & 1) { (va)->vx -= fxmul(dP_, (m)->anx); (va)->vy -= fxmul(dP_, (m)->any); (va)->w -= fxmul(dP_, ka); } \
    if ((m)->dyn & 2) { (vb)->vx += fxmul(dP_, (m)->bnx); (vb)->vy += fxmul(dP_, (m)->bny); (vb)->w += fxmul(dP_, kb); } \
  } } while (0)
#define APPLY_T(m, va, vb, dP, ka, kb) do { fx dP_ = (dP); if (dP_) { \
    if ((m)->dyn & 1) { (va)->vx += fxmul(dP_, (m)->any); (va)->vy -= fxmul(dP_, (m)->anx); (va)->w -= fxmul(dP_, ka); } \
    if ((m)->dyn & 2) { (vb)->vx -= fxmul(dP_, (m)->bny); (vb)->vy += fxmul(dP_, (m)->bnx); (vb)->w += fxmul(dP_, kb); } \
  } } while (0)

/* the solver passes run over nm manifolds from m (points from s) and return
   the number of points they used */
static int warm_start(SMan* m, int nm, SPt* s) {
  SPt* s0 = s;
  for (SMan* me = m + nm; m < me; m++) {
    BodyVel* va = &phys_vel[m->a]; BodyVel* vb = &phys_vel[m->b];
    for (int k = 0; k < m->n; k++, s++) {
      APPLY_N(m, va, vb, s->Pn, s->kna, s->knb);
      APPLY_T(m, va, vb, s->Pt, s->kta, s->ktb);
    }
  }
  return s - s0;
}

/* velocity bias: speculative for gaps, Baumgarte push-out (biased pass only) for overlap.
   Contacts stay rigid in velocity (no soft spring), so a tall column doesn't sag and
   buckle; the relax pass strips the push-out velocity again (split-impulse style). */
static inline fx contact_bias(fx sep, int use_bias) {
  if (sep > 0) return fxmul(sep, INV_H);
  if (!use_bias) return 0;
  fx b = fxmul(BAUMGARTE_RATE, sep + LINEAR_SLOP);
  if (b > 0) b = 0;
  if (b < -MAX_BIAS_VEL) b = -MAX_BIAS_VEL;
  return b;
}

/* relative motion at a contact along the normal (separation change, Q24 -> Q16;
   normal velocity) or tangent. A ground body has zero velocity and position
   change, so its terms are left out: most contacts are a moving block on a
   sleeping one. */
static inline fx rel_sep(const SMan* m, const SPt* s, const BodyVel* va, const BodyVel* vb) {
  fx nx = m->nx, ny = m->ny;
  if (m->dyn == 2) return (fx)(((s64)vb->dpx * nx + (s64)vb->dpy * ny + (s64)vb->dq * s->rn2 + 0x800000) >> 24);
  fx ddx = vb->dpx - va->dpx, ddy = vb->dpy - va->dpy;
  return (fx)(((s64)ddx * nx + (s64)ddy * ny + (s64)vb->dq * s->rn2 - (s64)va->dq * s->rn1 + 0x800000) >> 24);
}
static inline fx rel_vn(const SMan* m, const SPt* s, const BodyVel* va, const BodyVel* vb) {
  fx nx = m->nx, ny = m->ny;
  if (m->dyn == 2) return (fx)(((s64)vb->vx * nx + (s64)vb->vy * ny + (s64)vb->w * s->rn2 + 0x8000) >> 16);
  fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
  return (fx)(((s64)dvx * nx + (s64)dvy * ny + (s64)vb->w * s->rn2 - (s64)va->w * s->rn1 + 0x8000) >> 16);
}
static inline fx rel_vt(const SMan* m, const SPt* s, const BodyVel* va, const BodyVel* vb) {
  fx nx = m->nx, ny = m->ny;
  if (m->dyn == 2) return (fx)(((s64)vb->vy * nx - (s64)vb->vx * ny + (s64)vb->w * s->rt2 + 0x8000) >> 16);
  fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
  return (fx)(((s64)dvy * nx - (s64)dvx * ny + (s64)vb->w * s->rt2 - (s64)va->w * s->rt1 + 0x8000) >> 16);
}

__attribute__((noinline)) static void solve_block(SMan* m, SPt* s0, BodyVel* va, BodyVel* vb, int use_bias) {
  SPt* s1 = s0 + 1;
  fx vn0 = rel_vn(m, s0, va, vb) + contact_bias(s0->sep0 + rel_sep(m, s0, va, vb), use_bias);
  fx vn1 = rel_vn(m, s1, va, vb) + contact_bias(s1->sep0 + rel_sep(m, s1, va, vb), use_bias);
  fx a0 = s0->Pn, a1 = s1->Pn;
  fx b0 = vn0 - fxmul(m->k11, a0) - fxmul(m->k12, a1);
  fx b1 = vn1 - fxmul(m->k12, a0) - fxmul(m->k22, a1);
  fx x0, x1;
  for (;;) {
    /* both points active: x = -K^-1 b. In 64 bits: with a speculative point
       (large b) on a badly conditioned pair this overflowed 32 bits and the
       wrapped result could pass the sign test and fling the body away */
    s64 X0 = -((s64)m->i11 * b0 + (s64)m->i12 * b1), X1 = -((s64)m->i12 * b0 + (s64)m->i22 * b1);
    if (X0 >= 0 && X1 >= 0 && X0 < ((s64)1 << 44) && X1 < ((s64)1 << 44)) {
      x0 = (fx)((X0 + 0x8000) >> 16); x1 = (fx)((X1 + 0x8000) >> 16); break;
    }
    x0 = fxmul(-b0, s0->mn); x1 = 0;
    if (x0 >= 0 && fxmul(m->k12, x0) + b1 >= 0) break;
    x0 = 0; x1 = fxmul(-b1, s1->mn);
    if (x1 >= 0 && fxmul(m->k12, x1) + b0 >= 0) break;
    x0 = 0; x1 = 0;
    break;
  }
  s0->Pn = x0; s1->Pn = x1;
  if (x0 > s0->maxPn) s0->maxPn = x0;
  if (x1 > s1->maxPn) s1->maxPn = x1;
  APPLY_N(m, va, vb, x0 - a0, s0->kna, s0->knb);
  APPLY_N(m, va, vb, x1 - a1, s1->kna, s1->knb);
}

__attribute__((noinline)) static int solve_contacts(SMan* m, int nm, SPt* s0, int use_bias) {
  /* per manifold: all normal constraints first, then friction (Box2D v3 order).
     Interleaving normal+friction per point made flat contacts drift sideways. */
  SPt* sb = s0;
  for (SMan* me = m + nm; m < me; m++) {
    BodyVel* va = &phys_vel[m->a]; BodyVel* vb = &phys_vel[m->b];
    int n = m->n;
    if (m->blk) solve_block(m, s0, va, vb, use_bias);
    else for (SPt* s = s0; s < s0 + n; s++) {
      fx bias = contact_bias(s->sep0 + rel_sep(m, s, va, vb), use_bias);
      fx imp = -fxmul(s->mn, rel_vn(m, s, va, vb) + bias);
      fx Pn0 = s->Pn, Pn = Pn0 + imp;
      if (Pn < 0) Pn = 0;
      s->Pn = Pn; if (Pn > s->maxPn) s->maxPn = Pn;
      APPLY_N(m, va, vb, Pn - Pn0, s->kna, s->knb);
    }
    for (SPt* s = s0; s < s0 + n; s++) {
      if (!s->Pn && !s->Pt) continue;           /* no load and no friction yet: nothing to do (exact) */
      fx maxPt = fxmul(m->fric, s->Pn);
      fx Pt0 = s->Pt, Pt = Pt0 - fxmul(s->mt, rel_vt(m, s, va, vb));
      if (Pt > maxPt) Pt = maxPt; else if (Pt < -maxPt) Pt = -maxPt;
      s->Pt = Pt;
      APPLY_T(m, va, vb, Pt - Pt0, s->kta, s->ktb);
    }
    s0 += n;
  }
  return s0 - sb;
}

/* Bodies that just woke up rested on sleeping ones, which never updated the
   contact impulses between them: those still hold the load from when each body
   settled, not the weight of everything stacked on it since. Warm-starting a
   woken column from those makes it sag, bounce and come apart. Instead, seed
   each woken body's support contacts with its static load: its own mass plus
   its share of everything resting on it. */
static u16 wl_start[MAX_BODIES + 1] EWRAM_BSS, wl_list[MAX_ARB] EWRAM_BSS;   /* support arbiters per body */
static u8 wl_rem[MAX_BODIES] EWRAM_BSS, wl_queue[MAX_BODIES] EWRAM_BSS;
static fx wl_load[MAX_BODIES] EWRAM_BSS;
static s64 wl_mx[MAX_BODIES] EWRAM_BSS;
/* support impulse W (upward, on the upper body) as normal + friction parts */
ROM_CODE static void wl_seed(Arbiter* r, int c, fx W) {
  fx nx = (fx)r->nx << 2, ny = (fx)r->ny << 2;
  if (r->up < 0) { nx = -nx; ny = -ny; }       /* normal from the lower body up */
  fx pn = fxmul(W, ny), pt = fxmul(W, nx), lim = fxmul(FRICTION, pn);
  if (pt > lim) pt = lim; else if (pt < -lim) pt = -lim;
  r->Pn[c] += pn; r->Pt[c] += r->up < 0 ? -pt : pt;
}
ROM_CODE static void wake_loads(Arbiter* arbs, int narb, const u8* woke) {
  /* support lists (arbiters where the body is the upper one), and how many
     dynamic bodies rest on each body */
  for (int i = 0; i <= phys_count; i++) wl_start[i] = 0;
  for (int i = 0; i < phys_count; i++) wl_rem[i] = 0;
  for (int k = 0; k < narb; k++) {
    Arbiter* r = &arbs[k];
    if (!r->up) continue;
    int lo = r->up > 0 ? r->a : r->b, hi = r->up > 0 ? r->b : r->a;
    if (phys_bodies[hi].flags & BODY_STATIC) continue;
    wl_start[hi + 1]++; wl_rem[lo]++;
  }
  for (int i = 0; i < phys_count; i++) wl_start[i + 1] += wl_start[i];
  for (int k = 0; k < narb; k++) {
    Arbiter* r = &arbs[k];
    if (!r->up) continue;
    int hi = r->up > 0 ? r->b : r->a;
    if (phys_bodies[hi].flags & BODY_STATIC) continue;
    wl_list[wl_start[hi]++] = k;
  }
  for (int i = phys_count; i > 0; i--) wl_start[i] = wl_start[i - 1];
  wl_start[0] = 0;
  /* load = own mass + an equal share (per support contact) of the load of each
     body resting on it, top down: a body is done once everything on it is.
     (No recursion: a tall column would overflow the small IWRAM stack.) */
  int qh = 0, qt = 0;
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
    wl_load[i] = piece_defs[b->shape].mass; wl_mx[i] = (s64)wl_load[i] * b->p.x;
    if (!wl_rem[i]) wl_queue[qt++] = i;
  }
  while (qh < qt) {
    int u = wl_queue[qh++], ns = wl_start[u + 1] - wl_start[u];
    if (!ns) continue;
    fx share = wl_load[u] / ns; s64 mshare = ns == 1 ? wl_mx[u] : wl_mx[u] / ns;
    for (int q = wl_start[u]; q < wl_start[u + 1]; q++) {
      Arbiter* r = &arbs[wl_list[q]];
      int lo = r->up > 0 ? r->a : r->b;
      if (phys_bodies[lo].flags & BODY_STATIC) continue;
      wl_load[lo] += share; wl_mx[lo] += mshare;
      if (!--wl_rem[lo]) wl_queue[qt++] = lo;
    }
  }
  for (int i = 0; i < phys_count; i++) {
    int s0 = wl_start[i], s1 = wl_start[i + 1];
    if (!woke[i] || s0 == s1) continue;
    fx load = wl_load[i], xc = (fx)(wl_mx[i] / load);
    /* the static split over its support points: lever rule between the
       outermost two (all of it on one if the centre overhangs) */
    Arbiter* rmin = 0; Arbiter* rmax = 0; int kmin = 0, kmax = 0;
    for (int q = s0; q < s1; q++) {
      Arbiter* r = &arbs[wl_list[q]];
      for (int c = 0; c < r->n; c++) {
        r->Pn[c] = 0; r->Pt[c] = 0;
        if (!rmin || r->cx[c] < rmin->cx[kmin]) { rmin = r; kmin = c; }
        if (!rmax || r->cx[c] > rmax->cx[kmax]) { rmax = r; kmax = c; }
      }
    }
    fx P = fxmul(load, fxmul(GRAVITY, H));     /* g*H first: load*g overflows Q16 for a tall column */
    fx x0 = rmin->cx[kmin], x1 = rmax->cx[kmax], p1 = P / 2;
    if (x1 - x0 >= FX(0.05)) {
      fx t = (fx)(((s64)(xc - x0) * FX_ONE) / (x1 - x0));
      if (t < 0) t = 0; else if (t > FX_ONE) t = FX_ONE;
      p1 = fxmul(P, t);
    }
    wl_seed(rmin, kmin, P - p1); wl_seed(rmax, kmax, p1);
  }
}

ROM_CODE static void apply_restitution(SMan* m, int nm, SPt* s) {
  for (SMan* me = m + nm; m < me; m++) {
    for (int k = 0; k < m->n; k++, s++) {
      if (s->vn0 > -REST_VEL || s->maxPn == 0) continue;
      BodyVel* va = &phys_vel[m->a]; BodyVel* vb = &phys_vel[m->b];
      fx imp = -fxmul(s->mn, rel_vn(m, s, va, vb) + fxmul(REST, s->vn0));
      fx Pn0 = s->Pn, Pn = Pn0 + imp; if (Pn < 0) Pn = 0;
      s->Pn = Pn;
      APPLY_N(m, va, vb, Pn - Pn0, s->kna, s->knb);
    }
  }
}

ROM_CODE static void wake_body(int i) {
  Body* b = &phys_bodies[i];
  arb_rescan = 1;
  b->flags &= ~BODY_SLEEP; b->sleepTime = 0;
  phys_vel[i].vx = phys_vel[i].vy = phys_vel[i].w = 0;
}

/* Static balance test for the sleeping structure, run whenever something falls
   asleep (i.e. a new block has settled). For every block: does the combined centre
   of mass of the block and everything stacked on it lie over the span of its
   support contacts? If not, wake it; the bodies above follow via the wake rule. */
/* A body the balance test woke that then fell asleep again without moving is
   held up by something the test can't see (a block leaning on a neighbour, or
   wedged by its sides). Remember where it rested and leave it alone while it
   stays there: otherwise it, and everything stacked on it, wakes and re-settles
   every half second for the rest of the game, which is what made busy towers
   slow. */
static fx tip_x[MAX_BODIES] EWRAM_BSS, tip_y[MAX_BODIES] EWRAM_BSS, tip_a[MAX_BODIES] EWRAM_BSS;
#define TIP_STILL_POS FX(0.05)
#define TIP_STILL_ANG FX(0.02)
ROM_CODE static int tip_moved(int i) {
  const Body* b = &phys_bodies[i];
  fx dx = b->p.x - tip_x[i], dy = b->p.y - tip_y[i], da = b->a - tip_a[i];
  return dx > TIP_STILL_POS || dx < -TIP_STILL_POS || dy > TIP_STILL_POS || dy < -TIP_STILL_POS || da > TIP_STILL_ANG || da < -TIP_STILL_ANG;
}
ROM_CODE static void tip_note_sleep(int i) {
  if (tip_woke[i]) { tip_woke[i] = 0; tip_ok[i] = !tip_moved(i); }
  else if (tip_ok[i] && tip_moved(i)) tip_ok[i] = 0;
}

static u8 tip_above_n[MAX_BODIES] EWRAM_BSS, tip_above[MAX_BODIES][8] EWRAM_BSS;
static fx tip_smin[MAX_BODIES] EWRAM_BSS, tip_smax[MAX_BODIES] EWRAM_BSS;
static u8 tip_sup[MAX_BODIES] EWRAM_BSS, tip_stack[MAX_BODIES] EWRAM_BSS;
static u16 tip_mark[MAX_BODIES] EWRAM_BSS, tip_gen;   /* visited this generation */
/* runs once per settle, in pieces: tip_gather() collects each sleeping body's
   support span and what rests on it, tip_bodies() (below) tests the loads */
ROM_CODE static void tip_gather(void) {
  fx* smin = tip_smin; fx* smax = tip_smax; u8* hasSup = tip_sup;
  for (int i = 0; i < phys_count; i++) { hasSup[i] = 0; tip_above_n[i] = 0; smin[i] = 0x7FFFFFFF; smax[i] = -0x7FFFFFFF; }
  for (int k = 0; k < narb; k++) {
    const Arbiter* r = &arbs[k];
    if (!r->up) continue;
    int lo = r->up > 0 ? r->a : r->b, hi = r->up > 0 ? r->b : r->a;
    Body* L = &phys_bodies[lo]; Body* U = &phys_bodies[hi];
    if (!(U->flags & BODY_SLEEP) || (U->flags & BODY_STATIC) || !r->tm) continue;
    for (int c = 0; c < r->n; c++) if (r->tm & (1 << c)) {
      if (r->cx[c] < smin[hi]) smin[hi] = r->cx[c];
      if (r->cx[c] > smax[hi]) smax[hi] = r->cx[c];
    }
    hasSup[hi] = 1;
    if (!(L->flags & BODY_STATIC) && tip_above_n[lo] < 8) {
      int dup = 0;
      for (int q = 0; q < tip_above_n[lo]; q++) if (tip_above[lo][q] == hi) dup = 1;
      if (!dup) tip_above[lo][tip_above_n[lo]++] = hi;
    }
  }
}

/* ---- broad phase -----------------------------------------------------------
 * Every body's box (grown by its speculative margin) is kept, sorted by bottom
 * edge, from step to step: resting bodies don't move, so only the awake ones
 * are re-boxed and re-sorted (a nearly sorted list: one insertion-sort pass).
 * Pairs are then looked up around each awake body only, so a tall sleeping
 * tower costs next to nothing however many blocks it has. */
#define BP_MAX_CAND 512
static fx bp_x0[MAX_BODIES] EWRAM_BSS, bp_x1[MAX_BODIES] EWRAM_BSS, bp_y0[MAX_BODIES] EWRAM_BSS, bp_y1[MAX_BODIES] EWRAM_BSS;
static fx bp_key[MAX_BODIES] EWRAM_BSS;           /* bp_y0 in sorted order */
static u8 bp_pos[MAX_BODIES] EWRAM_BSS, bp_isaw[MAX_BODIES] EWRAM_BSS;
static u8 bp_pair[BP_MAX_CAND][2] EWRAM_BSS;
static int bp_np;
static fx bp_hmax;                                /* tallest box: how far below a body's bottom to look */
static u8 aw_list[MAX_BODIES];                    /* awake bodies this step (they are the ones that move) */
static int aw_n;
static fx speed[MAX_BODIES] EWRAM_BSS;

static void bp_box(int i) {
  Body* b = &phys_bodies[i];
  fx e = (MARGIN + 1) / 2 + fxmul(speed[i], DT) + 1;
  bp_x0[i] = b->bmin.x - e; bp_x1[i] = b->bmax.x + e; bp_y0[i] = b->bmin.y - e; bp_y1[i] = b->bmax.y + e;
  if (bp_y1[i] - bp_y0[i] > bp_hmax) bp_hmax = bp_y1[i] - bp_y0[i];
}

static void bp_sort(void) {
  for (int k = 1; k < bp_n; k++) {
    fx y = bp_key[k];
    if (bp_key[k - 1] <= y) continue;
    int i = bp_order[k], q = k - 1;
    while (q >= 0 && bp_key[q] > y) { bp_key[q + 1] = bp_key[q]; bp_order[q + 1] = bp_order[q]; bp_pos[bp_order[q + 1]] = q + 1; q--; }
    bp_key[q + 1] = y; bp_order[q + 1] = i; bp_pos[i] = q + 1;
  }
}

static void bp_add_pair(int i, int j) {
  if (bp_np >= BP_MAX_CAND) return;
  if (i > j) { int t = i; i = j; j = t; }
  bp_pair[bp_np][0] = i; bp_pair[bp_np][1] = j; bp_np++;
}

static void broadphase(void) {
  if (bp_dirty) {   /* bodies came or went: rebuild the list */
    int n = 0, pending = 0;
    bp_hmax = 0;
    for (int i = 0; i < phys_count; i++) {
      u8 f = phys_bodies[i].flags;
      bp_in[i] = 0;
      if (!(f & BODY_USED)) continue;
      if (f & BODY_NEW) { pending = 1; continue; }
      bp_box(i); bp_in[i] = 1;
      bp_order[n] = i; bp_key[n] = bp_y0[i]; bp_pos[i] = n; n++;
    }
    bp_n = n; bp_dirty = pending;
  } else
    for (int k = 0; k < aw_n; k++) { int i = aw_list[k]; bp_box(i); bp_key[bp_pos[i]] = bp_y0[i]; }
  bp_sort();
  bp_np = 0;
  for (int k = 0; k < aw_n; k++) bp_isaw[aw_list[k]] = 1;
  for (int k = 0; k < aw_n; k++) {
    int a = aw_list[k], p = bp_pos[a];
    fx bot = bp_y0[a], top = bp_y1[a], x0 = bp_x0[a], x1 = bp_x1[a];
    for (int q = p + 1; q < bp_n; q++) {           /* boxes starting above this one's bottom */
      if (bp_key[q] > top) break;
      int j = bp_order[q];
      if (bp_x0[j] > x1 || bp_x1[j] < x0) continue;
      bp_add_pair(a, j);
    }
    fx lim = bot - bp_hmax;
    for (int q = p - 1; q >= 0; q--) {             /* resting boxes starting below it (awake ones find it themselves) */
      if (bp_key[q] < lim) break;
      int j = bp_order[q];
      if (bp_isaw[j] || bp_y1[j] < bot || bp_x0[j] > x1 || bp_x1[j] < x0) continue;
      bp_add_pair(a, j);
    }
  }
}

/* ---- the step, in resumable pieces ---------------------------------------
 * A step is: update moved bodies' geometry, set aside the moving bodies'
 * contacts and run the broad phase (PRE), narrow phase per candidate pair,
 * waking + islands (ISL), SUBSTEPS x (gravity, warm start, solve, integrate,
 * relax), then the end of step (restitution, integrate positions, sleep,
 * balance check). Each piece is short, so the game can stop at a deadline and
 * carry on next frame. Positions only change at the end of a step, so whatever
 * is drawn in between is a whole step. */
enum { PH_IDLE, PH_UPDATE, PH_CARRY, PH_HASH, PH_BP, PH_NARROW, PH_WAKE, PH_ISL, PH_SOLVE, PH_PTS,
       PH_WL, PH_INT, PH_SUP, PH_SLEEP, PH_TIP, PH_TIPB, PH_FIN };
static int ph_c, ph_mi, ph_pi;
static u8 ph_sub, ph_pass, ph_it, ph_any, ph_unlimited;
static u16 ph_deadline;
static int nwoke;
static u8 woke[MAX_BODIES] EWRAM_BSS, woke_list[MAX_BODIES] EWRAM_BSS, sup_state[MAX_BODIES] EWRAM_BSS;
static s16 island_min[MAX_BODIES] EWRAM_BSS;
/* cost estimates for the pieces whose size is hard to predict: a whole piece
   (ticks), or a rate per item (1/16 ticks) for pieces that loop over a known
   count. Each follows the last measurement up at once and down slowly, so one
   costly step makes the next few careful. */
static u16 est_hash = 100, est_bp = 300, est_bpd = 600;
static u16 rate_carry = 6 * 16, rate_wake = 4 * 16, rate_isl = 6 * 16, rate_wl = 20 * 16, rate_sup = 4 * 16, rate_tip = 12 * 16;
#define EST(rate, n) (16 + (int)(((u32)(n) * (rate)) >> 4))

ROM_CODE static void body_reset_scratch(int i) {
  woke[i] = 0; wake_req[i] = 0; speed[i] = 0; uf_parent[i] = i; island_min[i] = 0x7FFF; bp_isaw[i] = 0; sup_state[i] = 0;
}

/* may a piece of about `est` ticks still run before the deadline? The first
   piece of a call always may (if any time is left), so a step always moves on */
#ifdef HOST
static inline int can_run(int est) { (void)est; return 1; }
static inline u16 tnow(void) { return 0; }
#else
static inline u16 tnow(void) { return REG_TM2CNT_L; }
static int can_run(int est) {
  if (ph_unlimited) return 1;
  s16 left = (s16)(ph_deadline - REG_TM2CNT_L);
  if (!ph_any) { ph_any = 1; return left > 0; }
  return left >= est;
}
#endif
static inline u16 measured(u16 t0) { u16 d = (u16)(tnow() - t0); return d + (d >> 3) + 8; }
static u16 track(u16 est, u16 t0) { u16 m = measured(t0); return m > est ? m : est - (est >> 4); }
static u16 track_rate(u16 rate, u16 t0, int n) {
  if (n <= 0) return rate;
  u32 r = ((u32)measured(t0) * 16 + n - 1) / n;
  if (r > 0xFFFF) r = 0xFFFF;
  return r > rate ? (u16)r : rate - (rate >> 4);
}

static SMan* man_at(int mi) { return mi < nman_iw ? &mans[mi] : &mans_x[mi - nman_iw]; }
static SPt* pt_at(int pi) { return pi < npts_iw ? &pts[pi] : &pts_x[pi - npts_iw]; }

/* is anything going to move? (else a step changes nothing and is skipped) */
ROM_CODE static int step_needed(void) {
  if (wr_n) return 1;
  for (int i = 0; i < phys_count; i++) {
    u8 f = phys_bodies[i].flags;
    if ((f & (BODY_USED | BODY_STATIC | BODY_SLEEP)) == BODY_USED) return 1;   /* awake, or new */
  }
  return 0;
}

ROM_CODE static void step_begin(void) {
  for (int i = 0; i < phys_count; i++) phys_bodies[i].flags &= ~BODY_NEW;
  npts = nman = nman_iw = npts_iw = 0;
  aw_n = 0;
  ph = PH_UPDATE; ph_c = 0;
}

/* keep resting bodies' contacts where they are, set aside the moving ones'
   (for warm starting): in pieces of CARRY_CHUNK arbiters */
#define CARRY_CHUNK 24
static int ph_w;
static void carry_begin(void) {
  int from = arb_rescan ? 0 : ncarried;
  arb_rescan = 0; nprev = 0; ph_c = ph_w = from;
}
__attribute__((noinline)) static int carry_piece(void) {
  int end = ph_c + CARRY_CHUNK, w = ph_w;
  if (end > narb) end = narb;
  for (int k = ph_c; k < end; k++) {
    Arbiter* r = &arbs[k];
    Body* A = &phys_bodies[r->a]; Body* B = &phys_bodies[r->b];
    if (!(A->flags & BODY_USED) || !(B->flags & BODY_USED)) continue;
    if (is_awake_dyn(A) || is_awake_dyn(B)) { prev_aw[nprev++] = *r; continue; }
    if (w != k) arbs[w] = *r;
    w++;
  }
  ph_c = end; ph_w = w;
  if (end < narb) return 0;
  narb = ncarried = w;
  return 1;
}

/* speeds, broad phase */
__attribute__((noinline)) static void step_bp(void) {
  for (int k = 0; k < aw_n; k++) {
    int i = aw_list[k]; BodyVel* v = &phys_vel[i];
    fx ax = v->vx < 0 ? -v->vx : v->vx, ay = v->vy < 0 ? -v->vy : v->vy, aw = v->w < 0 ? -v->w : v->w;
    speed[i] = ax + ay + fxmul(aw, FX(2.5)) + fxmul(GRAVITY, DT);
  }
  broadphase();
}

/* narrow phase for one candidate pair, from part pair ph_pp on: 1 done, 0 out
   of time (ph_pp says where to carry on), -1 the solver is full */
#define EST_PART_PAIR 180
static int ph_pp;
static int narrow_pair(int i, int j) {
  Body* A = &phys_bodies[i]; Body* B = &phys_bodies[j];
  /* speculative margin grows with speed so fast drops don't tunnel deep into the
     stack: with how fast the two could close in on each other (bounded by their
     relative velocity, plus what the step may add, CULL_VSLACK), so blocks that
     tumble down together don't collide with everything near them */
  fx sp = speed[i] + speed[j];
  {
    BodyVel* va = &phys_vel[i]; BodyVel* vb = &phys_vel[j];
    fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy, wa = va->w, wb = vb->w;
    fx rel = (dvx < 0 ? -dvx : dvx) + (dvy < 0 ? -dvy : dvy) + fxmul((wa < 0 ? -wa : wa) + (wb < 0 ? -wb : wb), FX(2.5)) + CULL_VSLACK;
    if (rel < sp) sp = rel;
  }
  fx mg = MARGIN + fxmul(sp, DT);
  PFN(9);
  V2 Amin = A->bmin, Amax = A->bmax, Bmin = B->bmin, Bmax = B->bmax;
  if (!aabb_overlap(Amin, Amax, Bmin, Bmax, mg)) return 1;
  sep_margin = mg;
  /* part boxes into IWRAM once per pair (EWRAM reads are slow), keeping
     only parts that reach the other body's box */
  V2 pmin[2][MAX_PARTS], pmax[2][MAX_PARTS]; unsigned ma = 0, mb = 0;
  for (int pa = 0; pa < A->nparts; pa++) {
    pmin[0][pa] = A->wp[pa].bmin; pmax[0][pa] = A->wp[pa].bmax;
    if (aabb_overlap(pmin[0][pa], pmax[0][pa], Bmin, Bmax, mg)) ma |= 1 << pa;
  }
  if (!ma) return 1;
  for (int pb = 0; pb < B->nparts; pb++) {
    pmin[1][pb] = B->wp[pb].bmin; pmax[1][pb] = B->wp[pb].bmax;
    if (aabb_overlap(pmin[1][pb], pmax[1][pb], Amin, Amax, mg)) mb |= 1 << pb;
  }
  for (int pa = 0; pa < A->nparts; pa++) if (ma & (1 << pa)) for (int pb = 0; pb < B->nparts; pb++) if (mb & (1 << pb)) {
    int q = pa * MAX_PARTS + pb;
    if (q < ph_pp) continue;                       /* done in an earlier piece */
    PFN(10);
    if (!aabb_overlap(pmin[0][pa], pmax[0][pa], pmin[1][pb], pmax[1][pb], mg)) continue;
    /* out of solver points (a huge pile-up): stop, rather than make arbiters
       whose impulses never get written back */
    if (npts > MAX_PTS - 2 || nman >= MAX_MAN) return -1;
    if (!can_run(EST_PART_PAIR)) { ph_pp = q; return 0; }
    Manifold m;
    PFN(8);
    PF0(tc);
    int hit = collide_fast(&A->wp[pa], &B->wp[pb], &m);
    PF1(1, tc);
    if (!hit) continue;
    /* The speculative margin grows with speed, so in a collapse most points are
       gaps between blocks that fly past each other: keep only those that could
       close during this step (approach speed plus CULL_VSLACK for what the step
       itself adds). Solving the rest cost most of a big pile's time and almost
       never pushed anything. */
    {
      int kk = 0;
      for (int k = 0; k < m.count; k++) {
        if (m.c[k].sep > MARGIN) {
          BodyVel* va = &phys_vel[i]; BodyVel* vb = &phys_vel[j];
          fx r1x = m.c[k].p.x - A->p.x, r1y = m.c[k].p.y - A->p.y, r2x = m.c[k].p.x - B->p.x, r2y = m.c[k].p.y - B->p.y;
          fx dvx = vb->vx - fxmul(vb->w, r2y) - va->vx + fxmul(va->w, r1y);
          fx dvy = vb->vy + fxmul(vb->w, r2x) - va->vy - fxmul(va->w, r1x);
          fx vn = fxmul(dvx, m.n.x) + fxmul(dvy, m.n.y);
          if (m.c[k].sep > fxmul((vn < 0 ? -vn : 0) + CULL_VSLACK, DT)) continue;
        }
        if (kk != k) m.c[kk] = m.c[k];
        kk++;
      }
      if (!kk) continue;
      m.count = kk;
    }
    if (narb >= MAX_ARB) return -1;
    Arbiter* na = &arbs[narb++];
    na->a = i; na->b = j; na->pa = pa; na->pb = pb;
    na->up = m.n.y > SUPPORT_NY ? 1 : m.n.y < -SUPPORT_NY ? -1 : 0;
    PF0(tf);
    Arbiter* old = find_prev(i, j, pa, pb);
    /* the part pair's reference face (and so its normal) can change between
       frames: only warm start from impulses along about the same normal */
    if (old && (s32)old->nx * (m.n.x >> 2) + (s32)old->ny * (m.n.y >> 2) < (s32)(0.9 * (1 << 28))) old = 0;
    PF1(3, tf);
    PF0(ta);
    SPt* sp = add_points(i, j, pa, pb, &m, na, old, arbs);
    PF1(2, ta);
    /* a hard hit wakes a sleeping body even from above */
    for (int k = 0; k < m.count; k++) if (sp[k].vn0 < -IMPACT_WAKE) {
      if (A->flags & BODY_SLEEP) request_wake(i);
      if (B->flags & BODY_SLEEP) request_wake(j);
    }
  }
  return 1;
}

/* waking + islands.
   Sleeping bodies under an awake one stay asleep and act as static ground: the
   solver only ever sees the few blocks that are actually moving, which keeps a
   tall settled tower rock steady and cheap. Anything resting ON an awake body
   wakes (transitively), a hard impact wakes what it hits, and the balance check (tip_bodies) wakes
   the part of a sleeping tower whose load no longer sits over its support. */
static int ph_from, ph_wp;
/* one wake pass (the first also handles wake requests); 1 when they are over */
ROM_CODE static int wake_pass(void) {
  if (ph_wp == 0) {
    nwoke = 0;
    for (int k = 0; k < wr_n; k++) {
      int i = wr_list[k]; wake_req[i] = 0;
      if ((phys_bodies[i].flags & (BODY_USED | BODY_STATIC | BODY_SLEEP)) == (BODY_USED | BODY_SLEEP)) { wake_body(i); woke[i] = 1; woke_list[nwoke++] = i; }
    }
    wr_n = 0;
    /* only this step's contacts can have a moving body under a resting one,
       unless something just woke */
    ph_from = nwoke ? 0 : ncarried;
  }
  int changed = 0;
  for (int k = ph_from; k < narb; k++) {
    Arbiter* r = &arbs[k];
    int lo, hi;
    if (r->up > 0) { lo = r->a; hi = r->b; } else if (r->up < 0) { lo = r->b; hi = r->a; } else continue;
    Body* L = &phys_bodies[lo]; Body* U = &phys_bodies[hi];
    if ((L->flags & (BODY_STATIC | BODY_SLEEP)) || !(U->flags & BODY_SLEEP) || (U->flags & BODY_STATIC)) continue;
    wake_body(hi); woke[hi] = 1; woke_list[nwoke++] = hi; changed = 1;
  }
  if (changed) ph_from = 0;
  return !changed || ++ph_wp >= 16;
}
/* islands of this step's moving bodies */
ROM_CODE static void step_islands(void) {
  for (int k = 0; k < aw_n; k++) {
    int i = aw_list[k];
    uf_parent[i] = i; island_min[i] = 0x7FFF;
    BodyVel* v = &phys_vel[i]; v->dpx = v->dpy = v->dq = 0;
  }
  for (int k = 0; k < nwoke; k++) uf_parent[woke_list[k]] = woke_list[k];
  for (int k = ncarried; k < narb; k++) {
    Arbiter* r = &arbs[k];
    if (!is_awake_dyn(&phys_bodies[r->a]) || !is_awake_dyn(&phys_bodies[r->b])) continue;
    uf_union(r->a, r->b);
  }
}

__attribute__((noinline)) static void step_gravity(void) {
  for (int k = 0; k < aw_n; k++) phys_vel[aw_list[k]].vy -= fxmul(GRAVITY, H);
}

__attribute__((noinline)) static void step_integrate_vel(void) {
  for (int k = 0; k < aw_n; k++) {
    BodyVel* v = &phys_vel[aw_list[k]];
    /* safety net: nothing in the game moves this fast, so a body that does
       came out of a solver glitch and would only fly off the screen */
    if (v->vx > MAX_VEL) v->vx = MAX_VEL; else if (v->vx < -MAX_VEL) v->vx = -MAX_VEL;
    if (v->vy > MAX_VEL) v->vy = MAX_VEL; else if (v->vy < -MAX_VEL) v->vy = -MAX_VEL;
    if (v->w > MAX_ANG_VEL) v->w = MAX_ANG_VEL; else if (v->w < -MAX_ANG_VEL) v->w = -MAX_ANG_VEL;
    v->dpx += (fx)(((s64)v->vx * H24 + 0x8000) >> 16); v->dpy += (fx)(((s64)v->vy * H24 + 0x8000) >> 16); v->dq += (fx)(((s64)v->w * H24 + 0x8000) >> 16);
  }
}

/* ---- end of step, in pieces: positions, sleep, balance check ---- */
/* integrate positions + sleep bookkeeping for aw_list[k0, k1) */
ROM_CODE static void integrate_bodies(int k0, int k1) {
  for (int k = k0; k < k1; k++) {
    int i = aw_list[k];
    Body* b = &phys_bodies[i];
    BodyVel* v = &phys_vel[i];
    /* dp/dq are Q24: keep the sub-Q16 remainder per body, otherwise slow rotations
       (w*dt below 1 LSB) never integrate and a tower shears apart while the solver
       believes it is rotating rigidly */
    { s32 t;
      t = v->dpx + b->frx; b->p.x += t >> 8; b->frx = t & 255;
      t = v->dpy + b->fry; b->p.y += t >> 8; b->fry = t & 255;
      t = v->dq + b->fra;  b->a += t >> 8;   b->fra = t & 255; }
    while (b->a > FX_PI) b->a -= 2 * FX_PI;
    while (b->a < -FX_PI) b->a += 2 * FX_PI;
    fx ax = v->vx < 0 ? -v->vx : v->vx, ay = v->vy < 0 ? -v->vy : v->vy, aw = v->w < 0 ? -v->w : v->w;
    if (ax < FX(1) && ay < FX(1) && fxmul(v->vx, v->vx) + fxmul(v->vy, v->vy) < SLEEP_LIN2 && aw < SLEEP_ANG) { if (b->sleepTime < 0x7FFF) b->sleepTime++; }
    else b->sleepTime = 0;
    int r = uf_find(i);
    if (b->sleepTime < island_min[r]) island_min[r] = b->sleepTime;
  }
}

/* a still body whose supports are all asleep (or static) can sleep on its own,
   even while things above it still move: a swaying tower then freezes from the
   bottom up, so only the part that is actually moving costs anything (and the
   frozen part is perfectly rigid ground for it). Only this step's contacts
   can involve a moving body. */
ROM_CODE static void support_states(void) {
  for (int k = 0; k < aw_n; k++) sup_state[aw_list[k]] = 0;   /* bit 0: rests on something, bit 1: on something awake */
#ifndef NO_BOTTOM_SLEEP
  for (int k = ncarried; k < narb; k++) {
    Arbiter* r = &arbs[k];
    if (!r->up) continue;
    int lo = r->up > 0 ? r->a : r->b, hi = r->up > 0 ? r->b : r->a;
    sup_state[hi] |= is_awake_dyn(&phys_bodies[lo]) ? 3 : 1;
  }
#endif
}

/* put still islands (and still bodies on resting ground) of aw_list[k0, k1) to sleep */
ROM_CODE static int sleep_bodies(int k0, int k1) {
  int slept = 0;
  for (int k = k0; k < k1; k++) {
    int i = aw_list[k];
    Body* b = &phys_bodies[i];
    bp_isaw[i] = 0;
    if (island_min[uf_find(i)] >= SLEEP_TIME || (b->sleepTime >= SLEEP_TIME && sup_state[i] == 1)) {
      b->flags |= BODY_SLEEP; phys_vel[i].vx = phys_vel[i].vy = phys_vel[i].w = 0; speed[i] = 0;
      tip_note_sleep(i);
      update_body(b);
      slept = 1;
    }
  }
  return slept;
}

/* balance test, body by body from ph_c (see tip_gather): does the centre of
   mass of each sleeping block and everything (transitively) resting on it lie
   over its support span? The walk up the stack can be long in a tall tower, so
   it can stop at the deadline and carry on: 1 when all are done */
#define TIP_NODES 16
#define EST_TIP_NODES 200          /* ticks for TIP_NODES bodies of the walk */
static int tip_sp; static s64 tip_mx, tip_mt; static u8 tip_busy;
ROM_CODE static int tip_bodies(void) {
  for (; ph_c < phys_count; ph_c++) {
    int i = ph_c;
    Body* b = &phys_bodies[i];
    if (!tip_busy) {
      if (!tip_sup[i] || (b->flags & (BODY_USED | BODY_STATIC | BODY_SLEEP)) != (BODY_USED | BODY_SLEEP)) continue;
      if (!can_run(EST_TIP_NODES)) return 0;
      if (++tip_gen == 0) { for (int q = 0; q < MAX_BODIES; q++) tip_mark[q] = 0; tip_gen = 1; }
      tip_sp = 0; tip_stack[tip_sp++] = i; tip_mark[i] = tip_gen;
      tip_mx = tip_mt = 0; tip_busy = 1;
    }
    for (int n = 0; tip_sp; n++) {
      if (n == TIP_NODES) { if (!can_run(EST_TIP_NODES)) return 0; n = 0; }
      int c = tip_stack[--tip_sp];
      fx mass = piece_defs[phys_bodies[c].shape].mass;
      tip_mx += (s64)mass * phys_bodies[c].p.x; tip_mt += mass;
      for (int q = 0; q < tip_above_n[c]; q++) { int u = tip_above[c][q]; if (tip_mark[u] != tip_gen) { tip_mark[u] = tip_gen; tip_stack[tip_sp++] = u; } }
    }
    tip_busy = 0;
    fx comx = (fx)(tip_mx / tip_mt);
    if ((comx < tip_smin[i] - TIP_MARGIN || comx > tip_smax[i] + TIP_MARGIN) && !tip_ok[i]) {
      wake_body(i);
      tip_woke[i] = 1; tip_x[i] = b->p.x; tip_y[i] = b->p.y; tip_a[i] = b->a;
    }
  }
  return 1;
}

ROM_CODE static void step_finish(void) {
  for (int k = 0; k < nwoke; k++) woke[woke_list[k]] = 0;
  phys_awake = aw_n; phys_points = npts;
}

#define SOLVE_CHUNK 8      /* manifolds per piece of a solver pass */
/* ticks per point for a piece of each pass (warm start, solve, relax) */
static const u8 est_pt[3] = { 3, 12, 11 };

/* one piece of solver pass `pass` (1 warm, 2 solve, 4 relax) from ph_mi/ph_pi:
   1 done a piece, 0 the pass is over, -1 out of time */
__attribute__((noinline)) static int solve_piece(int pass) {
  if (ph_mi >= nman) return 0;
  int end = ph_mi < nman_iw ? nman_iw : nman;
  if (end > ph_mi + SOLVE_CHUNK) end = ph_mi + SOLVE_CHUNK;
  if (!can_run(16 + 2 * SOLVE_CHUNK * est_pt[pass == 1 ? 0 : pass == 2 ? 1 : 2])) return -1;
  SMan* m = man_at(ph_mi); SPt* s = pt_at(ph_pi); int nm = end - ph_mi, np;
  if (pass == 1) { PF0(t); np = warm_start(m, nm, s); PF1(6, t); }
  else if (pass == 2) { PF0(t); np = solve_contacts(m, nm, s, 1); PF1(4, t); }
  else { PF0(t); np = solve_contacts(m, nm, s, 0); PF1(5, t); }
  ph_mi = end; ph_pi += np;
  return 1;
}

/* restitution and impulses back to the arbiters, a piece at a time like the
   solver passes */
ROM_CODE static int pts_piece(void) {
  if (ph_mi >= nman) return 0;
  int end = ph_mi < nman_iw ? nman_iw : nman;
  if (end > ph_mi + SOLVE_CHUNK) end = ph_mi + SOLVE_CHUNK;
  if (!can_run(16 + 2 * SOLVE_CHUNK * 12)) return -1;
  SMan* m = man_at(ph_mi); SPt* s = pt_at(ph_pi); SPt* s0 = s; int nm = end - ph_mi;
  apply_restitution(m, nm, s);
  for (SMan* me = m + nm; m < me; m++) {
    Arbiter* r = &arbs[m->arb];
    for (int k = 0; k < m->n; k++, s++) { r->Pn[k] = s->Pn; r->Pt[k] = s->Pt; }
  }
  ph_mi = end; ph_pi += s - s0;
  return 1;
}

static u8 ph_slept;
#define INT_CHUNK 16
#define SLEEP_CHUNK 8

/* work on the step until the deadline; 1 when it is finished. The dispatcher
   runs from ROM; the pieces that loop over many items are ARM code in IWRAM */
ROM_CODE static int step_advance(void) {
  for (;;) switch (ph) {
    case PH_IDLE: return 1;
    case PH_UPDATE:
      PROF(0);
      for (; ph_c < phys_count; ph_c++) if (is_awake_dyn(&phys_bodies[ph_c])) {
        if (!can_run(48)) return 0;
        PF0(t); update_body(&phys_bodies[ph_c]); PF1(7, t);
        aw_list[aw_n++] = ph_c;
      }
      PROF(1);
      carry_begin(); ph = PH_CARRY; break;
    case PH_CARRY: {
      int n = narb - ph_c; if (n > CARRY_CHUNK) n = CARRY_CHUNK;
      if (!can_run(EST(rate_carry, n))) return 0;
      u16 t0 = tnow(); int done = carry_piece(); rate_carry = track_rate(rate_carry, t0, n);
      if (done) ph = PH_HASH;
      break;
    }
    case PH_HASH: {
      if (!can_run(est_hash)) return 0;
      u16 t0 = tnow(); build_prev_hash(); est_hash = track(est_hash, t0);
      ph = PH_BP; break;
    }
    case PH_BP: {
      int dirty = bp_dirty;
      if (!can_run(dirty ? est_bpd : est_bp)) return 0;
      u16 t0 = tnow(); step_bp();
      if (dirty) est_bpd = track(est_bpd, t0); else est_bp = track(est_bp, t0);
      PROF(2);
      ph = PH_NARROW; ph_c = 0; ph_pp = 0; break;
    }
    case PH_NARROW:
      for (; ph_c < bp_np; ph_c++, ph_pp = 0) {
        int i = bp_pair[ph_c][0], j = bp_pair[ph_c][1];
        if (!ph_pp && !can_run(40)) return 0;
        int r = narrow_pair(i, j);
        if (!r) return 0;
        if (r < 0) break;
      }
      PROF(3);
      ph = PH_WAKE; ph_wp = 0; break;
    case PH_WAKE: {
      int n = ph_wp ? narb - ph_from : wr_n ? narb + wr_n : narb - ncarried;
      if (!can_run(EST(rate_wake, n))) return 0;
      u16 t0 = tnow(); int done = wake_pass(); rate_wake = track_rate(rate_wake, t0, n);
      if (done) ph = PH_ISL;
      break;
    }
    case PH_ISL: {
      int n = aw_n + nwoke + narb - ncarried;
      if (!can_run(EST(rate_isl, n))) return 0;
      u16 t0 = tnow(); step_islands(); rate_isl = track_rate(rate_isl, t0, n);
      PROF(4);
      ph = PH_SOLVE; ph_sub = 0; ph_pass = 0; ph_it = 0; break;
    }
    case PH_SOLVE:
      if (ph_sub == SUBSTEPS) { ph = PH_PTS; ph_mi = ph_pi = 0; break; }
      switch (ph_pass) {
        case 0: if (!can_run(16 + aw_n)) return 0; step_gravity(); ph_pass = 1; ph_mi = ph_pi = 0; break;
        case 1: { int r = solve_piece(1); if (r < 0) return 0; if (!r) { ph_pass = 2; ph_mi = ph_pi = 0; } break; }
        case 2: { int r = solve_piece(2); if (r < 0) return 0;
                  if (!r) { if (++ph_it < ITERS) ph_mi = ph_pi = 0; else { ph_it = 0; ph_pass = 3; } } break; }
        case 3: if (!can_run(16 + 2 * aw_n)) return 0; step_integrate_vel(); ph_pass = 4; ph_mi = ph_pi = 0; break;
        case 4: { int r = solve_piece(4); if (r < 0) return 0; if (!r) { ph_pass = 0; ph_sub++; } break; }
      }
      break;
    case PH_PTS: {
      int r = pts_piece(); if (r < 0) return 0;
      if (!r) { PROF(5); ph = PH_WL; }
      break;
    }
    case PH_WL:
#ifndef NO_WAKE_LOAD
      if (nwoke) {
        int n = narb + phys_count;
        if (!can_run(EST(rate_wl, n))) return 0;
        u16 t0 = tnow(); wake_loads(arbs, narb, woke); rate_wl = track_rate(rate_wl, t0, n);
      }
#endif
      ph = PH_INT; ph_c = 0; break;
    case PH_INT: {
      int end = ph_c + INT_CHUNK; if (end > aw_n) end = aw_n;
      if (!can_run(16 + 24 * (end - ph_c))) return 0;
      integrate_bodies(ph_c, end); ph_c = end;
      if (end == aw_n) ph = PH_SUP;
      break;
    }
    case PH_SUP: {
      int n = aw_n + narb - ncarried;
      if (!can_run(EST(rate_sup, n))) return 0;
      u16 t0 = tnow(); support_states(); rate_sup = track_rate(rate_sup, t0, n);
      ph = PH_SLEEP; ph_c = 0; ph_slept = 0; break;
    }
    case PH_SLEEP: {
      int end = ph_c + SLEEP_CHUNK; if (end > aw_n) end = aw_n;
      if (!can_run(16 + 72 * (end - ph_c))) return 0;
      ph_slept |= sleep_bodies(ph_c, end); ph_c = end;
      if (end == aw_n) ph = ph_slept ? PH_TIP : PH_FIN;
      break;
    }
    case PH_TIP: {
      int n = narb + phys_count;
      if (!can_run(EST(rate_tip, n))) return 0;
      u16 t0 = tnow(); tip_gather(); rate_tip = track_rate(rate_tip, t0, n);
      ph = PH_TIPB; ph_c = 0; tip_busy = 0; break;
    }
    case PH_TIPB:
      if (!tip_bodies()) return 0;
      ph = PH_FIN; break;
    case PH_FIN:
      if (!can_run(16 + nwoke)) return 0;
      step_finish();
      PROF(6);
      ph = PH_IDLE; return 1;
  }
}

int phys_busy(void) { return ph != PH_IDLE; }

int phys_run(u16 deadline, int may_start) {
  int r = 0;
  ph_deadline = deadline; ph_any = 0;
  if (ph == PH_IDLE) {
    if (!may_start) return 0;
    r |= PHYS_STARTED;
    if (!step_needed()) { phys_awake = 0; phys_points = 0; return r | PHYS_DONE; }   /* all asleep: nothing to do */
    step_begin();
  }
  if (step_advance()) r |= PHYS_DONE;
  return r;
}

void phys_step(void) {
  ph_unlimited = 1;
  if (ph != PH_IDLE) step_advance();      /* finish one in progress first */
  phys_run(0, 1);
  ph_unlimited = 0;
}

void phys_wake(int i) { request_wake(i); }

void phys_remove(int i) { phys_bodies[i].flags = 0; bp_dirty = 1; arb_rescan = 1; }
#ifdef HOST
Arbiter* dbg_arbs(int* n) { *n = narb; return arbs; }
#endif
