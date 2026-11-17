/* Fixed-point 2D rigid-body physics for STAK! GBA.
 *
 * Units: 1 cell = 0.025 m = 12 px. Q16.16 fixed point throughout.
 * Sequential-impulse solver (Box2D-lite style) with warm starting, convex
 * polygon parts (SAT + clipping), and island sleeping so a settled tower
 * costs almost nothing.
 */
#pragma once
#include "gba.h"

typedef s32 fx;
typedef struct { fx x, y; } V2;

#define FX_ONE 65536
#define FX(x) ((fx)((x) * 65536.0))
#define FX_PI 205887

INLINE fx fxmul(fx a, fx b) { return (fx)(((s64)a * b + 0x8000) >> 16); }   /* rounded: truncation biases the solver */
INLINE fx fxcross(V2 a, V2 b) { return (fx)(((s64)a.x * b.y - (s64)a.y * b.x + 0x8000) >> 16); }
INLINE fx fxdot(V2 a, V2 b) { return (fx)(((s64)a.x * b.x + (s64)a.y * b.y + 0x8000) >> 16); }

#define MAX_BODIES 128
#define MAX_PARTS 4
#define MAX_VERTS 4             /* parts are triangles or quads (tools/gen_pieces.py checks) */

#define BODY_USED   1
#define BODY_STATIC 2
#define BODY_SLEEP  4
#define BODY_NEW    8          /* added while a step was running: joins at the next one */

typedef struct { u8 n; V2 v[MAX_VERTS]; V2 nrm[MAX_VERTS]; V2 bmin, bmax; } Poly;

/* geometry + state (EWRAM) */
typedef struct {
  V2 p; fx a;            /* position (COM), angle in Q16 radians */
  fx c, s;               /* cos/sin of angle */
  V2 bmin, bmax;         /* body AABB */
  s16 sleepTime;
  u8 flags, shape, color, nparts;
  u8 frx, fry, fra, pad;  /* sub-LSB position/angle remainders (Q24 integration) */
  Poly wp[MAX_PARTS];    /* world-space parts */
} Body;

/* velocity state used by the solver (IWRAM) */
typedef struct { fx vx, vy, w, dpx, dpy, dq; } BodyVel;   /* dp*, dq: Q24 */

extern Body phys_bodies[MAX_BODIES];
extern BodyVel phys_vel[MAX_BODIES];
extern int phys_count;           /* highest used index + 1 */
extern int phys_awake;           /* awake dynamic bodies last step */
extern int phys_points;          /* solver contact points last step */

void phys_init(void);
/* The game runs physics in slices: phys_run() works on the current step until
   timer 2 reaches `deadline` (64-cycle ticks), starting a new step first if
   `may_start` and none is running. Returns PHYS_STARTED / PHYS_DONE bits. A
   step that does not fit in one frame carries on in the next, so a heavy pile
   slows the simulation down instead of the whole game. Bodies added while a step
   is running stay put until the next step. */
#define PHYS_STARTED 1
#define PHYS_DONE    2
int phys_run(u16 deadline, int may_start);
int phys_busy(void);
/* add a static box (base). returns body index */
int phys_add_static_box(fx cx, fx cy, fx hw, fx hh);
/* add a dynamic piece from piece_defs */
int phys_add_piece(int shape, int color, fx x, fx y, fx angle);
void phys_step(void);            /* one whole step (tests, benchmarks) */
/* take a body out of the world (between steps: !phys_busy()) */
void phys_remove(int i);
/* wake a sleeping body (and, through the usual rules, whatever rests on it) at the next step */
void phys_wake(int i);
/* compute world geometry for an arbitrary transform of a piece into polys[] */
int phys_piece_world(int shape, fx x, fx y, fx angle, Poly* polys);
/* does the given set of polys overlap any body? (held piece test) */
int phys_overlaps(const Poly* polys, int n, fx tolerance);
fx fx_sin(fx a);
fx fx_cos(fx a);
