/* Physics engine. Compiled as ARM code and placed in IWRAM (fast 32-bit bus). */
#include "physics.h"
#include "gen/pieces_gen.h"
#include "gen/tables_gen.h"

#ifdef HOST
#define ROM_CODE
#else
#define ROM_CODE __attribute__((section(".romcode"), noinline))
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
#define SUBSTEPS  4
#ifndef ITERS
#define ITERS 1
#endif
#define H24       ((1 << 24) / (60 * SUBSTEPS))   /* substep in Q24 seconds */
#define H         (FX_ONE / (60 * SUBSTEPS))
#define INV_H     ((60 * SUBSTEPS) * FX_ONE)
#define GRAVITY   FX(392.4)        /* 9.81 m/s^2 in cells/s^2 (1 cell = 2.5 cm) */
#define MAX_BIAS_VEL FX(60.0)
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
#define SLEEP_TIME 30
#define MAX_ARB   256
#define SUPPORT_NY FX(0.3)       /* contact normal steeper than this = resting on */
#define IMPACT_WAKE FX(10.0)     /* approach speed that wakes a sleeping body */
#define TIP_MARGIN FX(0.0)
#define MAX_PTS   96

Body phys_bodies[MAX_BODIES] EWRAM_BSS;
BodyVel phys_vel[MAX_BODIES];
int phys_count, phys_awake, phys_points;
u32 phys_prof[8];
#ifdef HOST
#define PROF(k)
#else
#define PROF(k) phys_prof[k] = REG_TM2CNT_L
#endif

/* persistent arbiters (ping-pong) for warm starting, EWRAM */
typedef struct { u8 a, b, pa, pb; u8 n; s8 up; u16 feat[2]; fx Pn[2], Pt[2]; } Arbiter;   /* up: +1 b rests on a, -1 a rests on b */
static Arbiter arb_buf[2][MAX_ARB] EWRAM_BSS;
static int arb_n[2];
static int arb_cur;
static u8 arb_hash[512] EWRAM_BSS;

/* per-step solver points, IWRAM */
typedef struct {
  u8 a, b; u16 arb; u8 k, blk, pad[2];
  fx nx, ny, rn1, rn2, rt1, rt2;
  fx Pn, Pt, mn, mt, fric;
  fx k11, k12, k22, i11, i12, i22;   /* 2-point block solver (on the first point of a pair) */
  fx ima, imb, kna, knb, kta, ktb;
  fx sep0, vn0, maxPn;
} SPoint;
static SPoint pts[MAX_PTS];
static int npts;

static u8 uf_parent[MAX_BODIES];
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
static fx max_separation(const Poly* A, const Poly* B, int* edge) {
  fx best = -0x7FFFFFFF; int bi = 0;
  for (int i = 0; i < A->n; i++) {
    fx nx = A->nrm[i].x, ny = A->nrm[i].y;
    fx vx = A->v[i].x, vy = A->v[i].y;
    fx si = 0x7FFFFFFF;
    for (int j = 0; j < B->n; j++) {
      fx d = (fx)(((s64)nx * (B->v[j].x - vx) + (s64)ny * (B->v[j].y - vy) + 0x8000) >> 16);
      if (d < si) si = d;
    }
    if (si > best) { best = si; bi = i; if (si > sep_margin) break; }   /* separating axis: done */
  }
  *edge = bi;
  return best;
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
    fx t = !den ? 0 : num < 0 ? -(fx)udiv32((u32)(-num) << 16, den) : (fx)udiv32((u32)num << 16, den);
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
void phys_init(void) {
  for (int i = 0; i < MAX_BODIES; i++) phys_bodies[i].flags = 0;
  phys_count = 0; arb_n[0] = arb_n[1] = 0; arb_cur = 0; npts = 0;
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
  Body* b = &phys_bodies[i];
  b->flags = BODY_USED | BODY_STATIC; b->shape = 0xFF; b->nparts = 1;
  b->p.x = cx; b->p.y = cy; b->a = 0; b->c = FX_ONE; b->s = 0;
  Poly* p = &b->wp[0]; p->n = 4;
  p->v[0] = (V2){ cx - hw, cy - hh }; p->v[1] = (V2){ cx + hw, cy - hh };
  p->v[2] = (V2){ cx + hw, cy + hh }; p->v[3] = (V2){ cx - hw, cy + hh };
  p->nrm[0] = (V2){ 0, -FX_ONE }; p->nrm[1] = (V2){ FX_ONE, 0 };
  p->nrm[2] = (V2){ 0, FX_ONE }; p->nrm[3] = (V2){ -FX_ONE, 0 };
  p->bmin = p->v[0]; p->bmax = p->v[2]; b->bmin = p->v[0]; b->bmax = p->v[2];
  BodyVel* v = &phys_vel[i]; v->vx = v->vy = v->w = 0; v->invM = 0; v->invI = 0;
  return i;
}

ROM_CODE int phys_add_piece(int shape, int color, fx x, fx y, fx angle) {
  int i = alloc_body(); if (i < 0) return -1;
  Body* b = &phys_bodies[i];
  const PieceDef* d = &piece_defs[shape];
  b->flags = BODY_USED; b->shape = shape; b->color = color; b->nparts = d->nparts;
  b->p.x = x; b->p.y = y; b->a = angle; b->sleepTime = 0; b->frx = b->fry = b->fra = 0;
  tip_woke[i] = tip_ok[i] = 0;
  update_body(b);
  BodyVel* v = &phys_vel[i];
  v->vx = v->vy = v->w = 0;
  v->invM = recip(d->mass); v->invI = recip(d->inertia);
  return i;
}

int phys_overlaps(const Poly* polys, int n, fx tol) {
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
  return ((a * 31 + b) * 7 + pa * 3 + pb * 5) & 511;
}

static void build_prev_hash(int prev) {
  for (int i = 0; i < 512; i++) arb_hash[i] = 0xFF;
  for (int i = 0; i < arb_n[prev] && i < 255; i++) {
    Arbiter* r = &arb_buf[prev][i];
    unsigned h = arb_key_hash(r->a, r->b, r->pa, r->pb);
    while (arb_hash[h] != 0xFF) h = (h + 1) & 511;
    arb_hash[h] = i;
  }
}
static Arbiter* find_prev(int prev, int a, int b, int pa, int pb) {
  unsigned h = arb_key_hash(a, b, pa, pb);
  while (arb_hash[h] != 0xFF) {
    Arbiter* r = &arb_buf[prev][arb_hash[h]];
    if (r->a == a && r->b == b && r->pa == pa && r->pb == pb) return r;
    h = (h + 1) & 511;
  }
  return 0;
}

static int uf_find(int x) {
  while (uf_parent[x] != x) { uf_parent[x] = uf_parent[uf_parent[x]]; x = uf_parent[x]; }
  return x;
}
static void uf_union(int a, int b) { a = uf_find(a); b = uf_find(b); if (a != b) uf_parent[a] = b; }

static inline int is_awake_dyn(const Body* b) { return (b->flags & (BODY_USED | BODY_STATIC | BODY_SLEEP)) == BODY_USED; }

static inline fx inv_scale(fx x, u32 r, int rs) {
  s64 v = (s64)x * r;
  return (fx)(rs >= 0 ? v >> rs : v << -rs);
}

static void add_points(int ai, int bi, int pa, int pb, const Manifold* m, Arbiter* na, Arbiter* old) {
  int ni = na - arb_buf[arb_cur];
  Body* A = &phys_bodies[ai]; Body* B = &phys_bodies[bi];
  BodyVel* va = &phys_vel[ai]; BodyVel* vb = &phys_vel[bi];
  fx fric = ((A->flags | B->flags) & BODY_STATIC) ? FRICTION_BASE : FRICTION;
  /* a sleeping body is solid ground for whatever touches it */
  fx imA = (A->flags & BODY_SLEEP) ? 0 : va->invM, iiA = (A->flags & BODY_SLEEP) ? 0 : va->invI;
  fx imB = (B->flags & BODY_SLEEP) ? 0 : vb->invM, iiB = (B->flags & BODY_SLEEP) ? 0 : vb->invI;
  fx nx = m->n.x, ny = m->n.y;
  for (int k = 0; k < m->count && npts < MAX_PTS; k++) {
    SPoint* s = &pts[npts++];
    s->a = ai; s->b = bi; s->arb = ni; s->k = k;
    s->nx = nx; s->ny = ny;
    fx r1x = m->c[k].p.x - A->p.x, r1y = m->c[k].p.y - A->p.y;
    fx r2x = m->c[k].p.x - B->p.x, r2y = m->c[k].p.y - B->p.y;
    /* cross products r x n and r x t, t = (-ny, nx) */
    s->rn1 = (fx)(((s64)r1x * ny - (s64)r1y * nx + 0x8000) >> 16);
    s->rn2 = (fx)(((s64)r2x * ny - (s64)r2y * nx + 0x8000) >> 16);
    s->rt1 = (fx)(((s64)r1x * nx + (s64)r1y * ny + 0x8000) >> 16);
    s->rt2 = (fx)(((s64)r2x * nx + (s64)r2y * ny + 0x8000) >> 16);
    s->ima = imA; s->imb = imB;
    s->kna = fxmul(iiA, s->rn1); s->knb = fxmul(iiB, s->rn2);
    s->kta = fxmul(iiA, s->rt1); s->ktb = fxmul(iiB, s->rt2);
    s->fric = fric;
    s->Pn = 0; s->Pt = 0;
    if (old) {
      if (old->n == m->count) { s->Pn = old->Pn[k]; s->Pt = old->Pt[k]; }
      else for (int j = 0; j < old->n; j++) if (old->feat[j] == m->c[k].feat) { s->Pn = old->Pn[j]; s->Pt = old->Pt[j]; }
    }
    na->feat[k] = m->c[k].feat;
    fx kn = imA + imB + fxmul(s->kna, s->rn1) + fxmul(s->knb, s->rn2);
    fx kt = imA + imB + fxmul(s->kta, s->rt1) + fxmul(s->ktb, s->rt2);
    s->mn = recip(kn); s->mt = recip(kt);
    s->sep0 = m->c[k].sep;
    s->vn0 = (fx)(((s64)(vb->vx - va->vx) * nx + (s64)(vb->vy - va->vy) * ny + (s64)vb->w * s->rn2 - (s64)va->w * s->rn1 + 0x8000) >> 16);
    s->maxPn = 0;
    s->blk = 0;
  }
  na->n = m->count;
  if (m->count == 2 && npts >= 2 && pts[npts - 2].arb == ni && pts[npts - 1].arb == ni) {
    SPoint* s0 = &pts[npts - 2]; SPoint* s1 = &pts[npts - 1];
    fx im = s0->ima + s0->imb;
    fx k11 = im + fxmul(s0->kna, s0->rn1) + fxmul(s0->knb, s0->rn2);
    fx k22 = im + fxmul(s1->kna, s1->rn1) + fxmul(s1->knb, s1->rn2);
    fx k12 = im + fxmul(s0->kna, s1->rn1) + fxmul(s0->knb, s1->rn2);
    s64 det = (s64)k11 * k22 - (s64)k12 * k12;            /* Q32 */
    if (det > 0 && (s64)k11 * k11 < det * 500) {
      s0->blk = 1;
      s0->k11 = k11; s0->k12 = k12; s0->k22 = k22;
      /* inverse of K = adj(K) / det. One 32-bit reciprocal of the normalised det
         (17 significant bits) instead of three 64-bit divisions, which libgcc
         does in slow Thumb code from ROM */
      int sh = 0; u64 d = (u64)det;
      while (d >= (1ull << 31)) { d >>= 1; sh++; }
      while (d < (1u << 30)) { d <<= 1; sh--; }
      u32 r = udiv32(0xFFFFFFFFu, (u32)(d >> 15));        /* ~ 2^47 / d */
      int rs = 15 + sh;                              /* x * 2^32 / det = x * r >> rs */
      s0->i11 = inv_scale(k22, r, rs);
      s0->i12 = -inv_scale(k12, r, rs);
      s0->i22 = inv_scale(k11, r, rs);
    }
  }
}

static inline void apply(SPoint* s, BodyVel* va, BodyVel* vb, fx dP, fx dx, fx dy, fx ka, fx kb) {
  /* zero impulse (a clamped or idle contact) or a body acting as ground
     (inverse mass 0, so ka/kb are 0 too) changes nothing: skip the multiplies */
  if (!dP) return;
  if (s->ima) {
    fx p = fxmul(dP, s->ima);
    va->vx -= fxmul(p, dx); va->vy -= fxmul(p, dy); va->w -= fxmul(dP, ka);
  }
  if (s->imb) {
    fx p = fxmul(dP, s->imb);
    vb->vx += fxmul(p, dx); vb->vy += fxmul(p, dy); vb->w += fxmul(dP, kb);
  }
}

static void warm_start(void) {
  for (int i = 0; i < npts; i++) {
    SPoint* s = &pts[i];
    BodyVel* va = &phys_vel[s->a]; BodyVel* vb = &phys_vel[s->b];
    apply(s, va, vb, s->Pn, s->nx, s->ny, s->kna, s->knb);
    apply(s, va, vb, s->Pt, -s->ny, s->nx, s->kta, s->ktb);
  }
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

__attribute__((noinline)) static void solve_contacts(int use_bias) {
  /* per manifold: all normal constraints first, then friction (Box2D v3 order).
     Interleaving normal+friction per point made flat contacts drift sideways. */
  for (int i0 = 0; i0 < npts; ) {
    int i1 = i0 + 1;
    while (i1 < npts && pts[i1].arb == pts[i0].arb) i1++;
    BodyVel* va = &phys_vel[pts[i0].a]; BodyVel* vb = &phys_vel[pts[i0].b];
    fx nx = pts[i0].nx, ny = pts[i0].ny;
    if (i1 - i0 == 2 && pts[i0].blk) {
      SPoint* s0 = &pts[i0]; SPoint* s1 = &pts[i0 + 1];
      fx ddx = vb->dpx - va->dpx, ddy = vb->dpy - va->dpy;
      fx sp0 = s0->sep0 + (fx)(((s64)ddx * nx + (s64)ddy * ny + (s64)vb->dq * s0->rn2 - (s64)va->dq * s0->rn1 + 0x800000) >> 24);
      fx sp1 = s1->sep0 + (fx)(((s64)ddx * nx + (s64)ddy * ny + (s64)vb->dq * s1->rn2 - (s64)va->dq * s1->rn1 + 0x800000) >> 24);
      fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
      fx vn0 = (fx)(((s64)dvx * nx + (s64)dvy * ny + (s64)vb->w * s0->rn2 - (s64)va->w * s0->rn1 + 0x8000) >> 16);
      fx vn1 = (fx)(((s64)dvx * nx + (s64)dvy * ny + (s64)vb->w * s1->rn2 - (s64)va->w * s1->rn1 + 0x8000) >> 16);
      vn0 += contact_bias(sp0, use_bias);
      vn1 += contact_bias(sp1, use_bias);
      fx a0 = s0->Pn, a1 = s1->Pn;
      fx b0 = vn0 - fxmul(s0->k11, a0) - fxmul(s0->k12, a1);
      fx b1 = vn1 - fxmul(s0->k12, a0) - fxmul(s0->k22, a1);
      fx x0, x1;
      for (;;) {
        x0 = -fxmul(s0->i11, b0) - fxmul(s0->i12, b1);
        x1 = -fxmul(s0->i12, b0) - fxmul(s0->i22, b1);
        if (x0 >= 0 && x1 >= 0) break;
        x0 = fxmul(-b0, s0->mn); x1 = 0;
        if (x0 >= 0 && fxmul(s0->k12, x0) + b1 >= 0) break;
        x0 = 0; x1 = fxmul(-b1, s1->mn);
        if (x1 >= 0 && fxmul(s0->k12, x1) + b0 >= 0) break;
        x0 = 0; x1 = 0;
        break;
      }
      s0->Pn = x0; s1->Pn = x1;
      if (x0 > s0->maxPn) s0->maxPn = x0;
      if (x1 > s1->maxPn) s1->maxPn = x1;
      apply(s0, va, vb, x0 - a0, nx, ny, s0->kna, s0->knb);
      apply(s1, va, vb, x1 - a1, nx, ny, s1->kna, s1->knb);
    } else
    for (int i = i0; i < i1; i++) {
      SPoint* s = &pts[i];
      /* current separation from the positional change within this frame */
      fx ddx = vb->dpx - va->dpx, ddy = vb->dpy - va->dpy;
      fx sep = s->sep0 + (fx)(((s64)ddx * nx + (s64)ddy * ny + (s64)vb->dq * s->rn2 - (s64)va->dq * s->rn1 + 0x800000) >> 24);
      fx bias = contact_bias(sep, use_bias), mass = s->mn, iscale = 0;
      fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
      fx vn = (fx)(((s64)dvx * nx + (s64)dvy * ny + (s64)vb->w * s->rn2 - (s64)va->w * s->rn1 + 0x8000) >> 16);
      fx imp = -fxmul(mass, vn + bias) - fxmul(iscale, s->Pn);
      fx Pn0 = s->Pn, Pn = Pn0 + imp;
      if (Pn < 0) Pn = 0;
      s->Pn = Pn; if (Pn > s->maxPn) s->maxPn = Pn;
      apply(s, va, vb, Pn - Pn0, nx, ny, s->kna, s->knb);
    }
    for (int i = i0; i < i1; i++) {
      SPoint* s = &pts[i];
      fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
      fx vt = (fx)(((s64)dvy * nx - (s64)dvx * ny + (s64)vb->w * s->rt2 - (s64)va->w * s->rt1 + 0x8000) >> 16);
      fx maxPt = fxmul(s->fric, s->Pn);
      fx Pt0 = s->Pt, Pt = Pt0 - fxmul(s->mt, vt);
      if (Pt > maxPt) Pt = maxPt; else if (Pt < -maxPt) Pt = -maxPt;
      s->Pt = Pt;
      apply(s, va, vb, Pt - Pt0, -ny, nx, s->kta, s->ktb);
    }
    i0 = i1;
  }
}

static void apply_restitution(void) {
  for (int i = 0; i < npts; i++) {
    SPoint* s = &pts[i];
    if (s->vn0 > -REST_VEL || s->maxPn == 0) continue;
    BodyVel* va = &phys_vel[s->a]; BodyVel* vb = &phys_vel[s->b];
    fx dvx = vb->vx - va->vx, dvy = vb->vy - va->vy;
    fx vn = (fx)(((s64)dvx * s->nx + (s64)dvy * s->ny + (s64)vb->w * s->rn2 - (s64)va->w * s->rn1 + 0x8000) >> 16);
    fx imp = -fxmul(s->mn, vn + fxmul(REST, s->vn0));
    fx Pn0 = s->Pn, Pn = Pn0 + imp; if (Pn < 0) Pn = 0;
    s->Pn = Pn;
    apply(s, va, vb, Pn - Pn0, s->nx, s->ny, s->kna, s->knb);
  }
}

static void wake_body(int i) {
  Body* b = &phys_bodies[i];
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
static int tip_moved(int i) {
  const Body* b = &phys_bodies[i];
  fx dx = b->p.x - tip_x[i], dy = b->p.y - tip_y[i], da = b->a - tip_a[i];
  return dx > TIP_STILL_POS || dx < -TIP_STILL_POS || dy > TIP_STILL_POS || dy < -TIP_STILL_POS || da > TIP_STILL_ANG || da < -TIP_STILL_ANG;
}
static void tip_note_sleep(int i) {
  if (tip_woke[i]) { tip_woke[i] = 0; tip_ok[i] = !tip_moved(i); }
  else if (tip_ok[i] && tip_moved(i)) tip_ok[i] = 0;
}

static u8 tip_above_n[MAX_BODIES] EWRAM_BSS, tip_above[MAX_BODIES][8] EWRAM_BSS;
static fx tip_smin[MAX_BODIES] EWRAM_BSS, tip_smax[MAX_BODIES] EWRAM_BSS;
static u8 tip_sup[MAX_BODIES] EWRAM_BSS, tip_mark[MAX_BODIES] EWRAM_BSS, tip_stack[MAX_BODIES] EWRAM_BSS;
/* runs once per settle; in IWRAM so a block landing does not cost a frame */
static void tip_check(const Arbiter* arbs, int narb) {
  fx* smin = tip_smin; fx* smax = tip_smax; u8* hasSup = tip_sup; u8* mark = tip_mark; u8* stack = tip_stack;
  for (int i = 0; i < phys_count; i++) { hasSup[i] = 0; tip_above_n[i] = 0; smin[i] = 0x7FFFFFFF; smax[i] = -0x7FFFFFFF; }
  sep_margin = MARGIN;
  for (int k = 0; k < narb; k++) {
    const Arbiter* r = &arbs[k];
    if (!r->up) continue;
    int lo = r->up > 0 ? r->a : r->b, hi = r->up > 0 ? r->b : r->a;
    Body* L = &phys_bodies[lo]; Body* U = &phys_bodies[hi];
    if (!(U->flags & BODY_SLEEP) || (U->flags & BODY_STATIC)) continue;
    Manifold m;
    if (!collide(&phys_bodies[r->a].wp[r->pa], &phys_bodies[r->b].wp[r->pb], &m)) continue;
    for (int c = 0; c < m.count; c++) {
      if (m.c[c].p.x < smin[hi]) smin[hi] = m.c[c].p.x;
      if (m.c[c].p.x > smax[hi]) smax[hi] = m.c[c].p.x;
    }
    hasSup[hi] = 1;
    if (!(L->flags & BODY_STATIC) && tip_above_n[lo] < 8) {
      int dup = 0;
      for (int q = 0; q < tip_above_n[lo]; q++) if (tip_above[lo][q] == hi) dup = 1;
      if (!dup) tip_above[lo][tip_above_n[lo]++] = hi;
    }
  }
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if (!hasSup[i] || (b->flags & (BODY_USED | BODY_STATIC | BODY_SLEEP)) != (BODY_USED | BODY_SLEEP)) continue;
    /* load = this block + everything (transitively) resting on it */
    for (int q = 0; q < phys_count; q++) mark[q] = 0;
    int sp = 0; stack[sp++] = i; mark[i] = 1;
    s64 mx = 0; s64 mt = 0;
    while (sp) {
      int c = stack[--sp];
      fx mass = piece_defs[phys_bodies[c].shape].mass;
      mx += (s64)mass * phys_bodies[c].p.x; mt += mass;
      for (int q = 0; q < tip_above_n[c]; q++) { int u = tip_above[c][q]; if (!mark[u]) { mark[u] = 1; stack[sp++] = u; } }
    }
    fx comx = (fx)(mx / mt);
    if ((comx < smin[i] - TIP_MARGIN || comx > smax[i] + TIP_MARGIN) && !tip_ok[i]) {
      wake_body(i);
      tip_woke[i] = 1; tip_x[i] = b->p.x; tip_y[i] = b->p.y; tip_a[i] = b->a;
    }
  }
}

void phys_step(void) {
  int prev = arb_cur; arb_cur ^= 1;
  Arbiter* out = arb_buf[arb_cur]; int nout = 0;
  build_prev_hash(prev);
  npts = 0;

  PROF(0);
  /* 1. geometry for awake bodies */
  for (int i = 0; i < phys_count; i++) if (is_awake_dyn(&phys_bodies[i])) update_body(&phys_bodies[i]);

  PROF(1);
  /* 2. carry over arbiters between non-awake bodies (keeps islands connected) */
  for (int i = 0; i < arb_n[prev]; i++) {
    Arbiter* r = &arb_buf[prev][i];
    Body* A = &phys_bodies[r->a]; Body* B = &phys_bodies[r->b];
    if (!(A->flags & BODY_USED) || !(B->flags & BODY_USED)) continue;
    if (is_awake_dyn(A) || is_awake_dyn(B)) continue;
    if (nout < MAX_ARB) out[nout++] = *r;
  }

  PROF(2);
  /* 3. narrow phase for pairs with at least one awake dynamic body */
  static u8 wake_req[MAX_BODIES];
  for (int i = 0; i < phys_count; i++) wake_req[i] = 0;
  static fx speed[MAX_BODIES];
  for (int i = 0; i < phys_count; i++) {
    BodyVel* v = &phys_vel[i];
    if (!is_awake_dyn(&phys_bodies[i])) { speed[i] = 0; continue; }
    fx ax = v->vx < 0 ? -v->vx : v->vx, ay = v->vy < 0 ? -v->vy : v->vy, aw = v->w < 0 ? -v->w : v->w;
    speed[i] = ax + ay + fxmul(aw, FX(2.5)) + fxmul(GRAVITY, DT);
  }
  for (int i = 0; i < phys_count; i++) {
    Body* A = &phys_bodies[i];
    if (!(A->flags & BODY_USED)) continue;
    int aw = is_awake_dyn(A);
    for (int j = i + 1; j < phys_count; j++) {
      Body* B = &phys_bodies[j];
      if (!(B->flags & BODY_USED)) continue;
      if (!aw && !is_awake_dyn(B)) continue;
      /* speculative margin grows with speed so fast drops don't tunnel deep into the stack */
      fx mg = MARGIN + fxmul(speed[i] + speed[j], DT);
      if (!aabb_overlap(A->bmin, A->bmax, B->bmin, B->bmax, mg)) continue;
      sep_margin = mg;
      for (int pa = 0; pa < A->nparts; pa++) for (int pb = 0; pb < B->nparts; pb++) {
        if (!aabb_overlap(A->wp[pa].bmin, A->wp[pa].bmax, B->wp[pb].bmin, B->wp[pb].bmax, mg)) continue;
        Manifold m;
        if (!collide_fast(&A->wp[pa], &B->wp[pb], &m)) continue;
        if (nout >= MAX_ARB) continue;
        Arbiter* na = &out[nout++];
        na->a = i; na->b = j; na->pa = pa; na->pb = pb;
        na->up = m.n.y > SUPPORT_NY ? 1 : m.n.y < -SUPPORT_NY ? -1 : 0;
        int p0 = npts;
        add_points(i, j, pa, pb, &m, na, find_prev(prev, i, j, pa, pb));
        /* a hard hit wakes a sleeping body even from above */
        for (int k = p0; k < npts; k++) if (pts[k].vn0 < -IMPACT_WAKE) {
          if (A->flags & BODY_SLEEP) wake_req[i] = 1;
          if (B->flags & BODY_SLEEP) wake_req[j] = 1;
        }
      }
    }
  }
  arb_n[arb_cur] = nout;

  PROF(3);
  /* 4. waking + islands.
     Sleeping bodies under an awake one stay asleep and act as static ground: the
     solver only ever sees the few blocks that are actually moving, which keeps a
     tall settled tower rock steady and cheap. Anything resting ON an awake body
     wakes (transitively), a hard impact wakes what it hits, and tip_check() wakes
     the part of a sleeping tower whose load no longer sits over its support. */
  static u8 woke[MAX_BODIES];
  for (int i = 0; i < phys_count; i++) {
    woke[i] = 0;
    if (wake_req[i] && (phys_bodies[i].flags & BODY_SLEEP)) wake_body(i), woke[i] = 1;
  }
  for (int pass = 0, changed = 1; changed && pass < 16; pass++) {
    changed = 0;
    for (int k = 0; k < nout; k++) {
      Arbiter* r = &out[k];
      int lo, hi;
      if (r->up > 0) { lo = r->a; hi = r->b; } else if (r->up < 0) { lo = r->b; hi = r->a; } else continue;
      Body* L = &phys_bodies[lo]; Body* U = &phys_bodies[hi];
      if ((L->flags & (BODY_STATIC | BODY_SLEEP)) || !(U->flags & BODY_SLEEP) || (U->flags & BODY_STATIC)) continue;
      wake_body(hi); woke[hi] = 1; changed = 1;
    }
  }
  for (int i = 0; i < phys_count; i++) uf_parent[i] = i;
  for (int i = 0; i < nout; i++) {
    Body* A = &phys_bodies[out[i].a]; Body* B = &phys_bodies[out[i].b];
    if (!is_awake_dyn(A) || !is_awake_dyn(B)) continue;
    uf_union(out[i].a, out[i].b);
  }
  static s16 island_min[MAX_BODIES];
  for (int i = 0; i < phys_count; i++) island_min[i] = 0x7FFF;

  PROF(4);
  /* 5. solve (substepped) */
  int awake = 0;
  static u8 dyn[MAX_BODIES];
  for (int i = 0; i < phys_count; i++) {
    BodyVel* v = &phys_vel[i];
    v->dpx = v->dpy = v->dq = 0;
    dyn[i] = is_awake_dyn(&phys_bodies[i]) && !woke[i];
    awake += dyn[i];
  }
  for (int sub = 0; sub < SUBSTEPS; sub++) {
    for (int i = 0; i < phys_count; i++) if (dyn[i]) phys_vel[i].vy -= fxmul(GRAVITY, H);
    warm_start();
    for (int it = 0; it < ITERS; it++) solve_contacts(1);
    for (int i = 0; i < phys_count; i++) if (dyn[i]) {
      BodyVel* v = &phys_vel[i];
      v->dpx += (fx)(((s64)v->vx * H24 + 0x8000) >> 16); v->dpy += (fx)(((s64)v->vy * H24 + 0x8000) >> 16); v->dq += (fx)(((s64)v->w * H24 + 0x8000) >> 16);
    }
    solve_contacts(0);
  }
  apply_restitution();
  /* write back accumulated impulses */
  for (int i = 0; i < npts; i++) { Arbiter* r = &out[pts[i].arb]; r->Pn[pts[i].k] = pts[i].Pn; r->Pt[pts[i].k] = pts[i].Pt; }

  PROF(5);
  /* 6. integrate + sleep bookkeeping */
  int slept = 0;
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if (!is_awake_dyn(b) || woke[i]) continue;
    BodyVel* v = &phys_vel[i];
    /* dp/dq are Q24: keep the sub-Q16 remainder per body, otherwise slow rotations
       (w*dt below 1 LSB) never integrate and a tower shears apart while the solver
       believes it is rotating rigidly */
    { s32 t;
      t = v->dpx + b->frx; b->p.x += t >> 8; b->frx = t & 255;
      t = v->dpy + b->fry; b->p.y += t >> 8; b->fry = t & 255;
      t = v->dq + b->fra;  b->a += t >> 8;   b->fra = t & 255; }
    if (b->a > FX_PI) b->a -= 2 * FX_PI; else if (b->a < -FX_PI) b->a += 2 * FX_PI;
    fx l2 = fxmul(v->vx, v->vx) + fxmul(v->vy, v->vy);
    fx aw = v->w < 0 ? -v->w : v->w;
    if (l2 < SLEEP_LIN2 && aw < SLEEP_ANG) { if (b->sleepTime < 0x7FFF) b->sleepTime++; }
    else b->sleepTime = 0;
    int r = uf_find(i);
    if (b->sleepTime < island_min[r]) island_min[r] = b->sleepTime;
  }
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if (!is_awake_dyn(b) || woke[i]) continue;
    if (island_min[uf_find(i)] >= SLEEP_TIME) {
      b->flags |= BODY_SLEEP; phys_vel[i].vx = phys_vel[i].vy = phys_vel[i].w = 0;
      tip_note_sleep(i);
      update_body(b);
      slept = 1;
    }
  }
  if (slept) tip_check(out, nout);
  PROF(6);
  phys_awake = awake; phys_points = npts;
}

void phys_remove(int i) { phys_bodies[i].flags = 0; }
