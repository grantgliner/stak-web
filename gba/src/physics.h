/* Fixed-point 2D rigid-body physics for STAK! GBA.
 *
 * Units: 1 cell = 0.025 m = 12 px. Q16.16 fixed point throughout.
 * Sequential-impulse solver (Box2D-lite style) with warm starting, convex
 * polygon parts (SAT + clipping) and circle parts (with rolling resistance),
 * and island sleeping so a settled tower costs almost nothing.
 */
#pragma once
#include "gba.h"
#include "gen/pieces_gen.h"

typedef s32 fx;
typedef struct { fx x, y; } V2;

#define FX_ONE 65536
#define FX(x) ((fx)((x) * 65536.0))
#define FX_PI 205887

INLINE fx fxmul(fx a, fx b) { return (fx)(((s64)a * b + 0x8000) >> 16); }   /* rounded: truncation biases the solver */
INLINE fx fxcross(V2 a, V2 b) { return (fx)(((s64)a.x * b.y - (s64)a.y * b.x + 0x8000) >> 16); }
INLINE fx fxdot(V2 a, V2 b) { return (fx)(((s64)a.x * b.x + (s64)a.y * b.y + 0x8000) >> 16); }

#define MAX_BODIES 128
#define MAX_PARTS 5
#define MAX_VERTS 4             /* parts are triangles or quads (tools/gen_pieces.py checks) */

#define BODY_USED   1
#define BODY_STATIC 2
#define BODY_SLEEP  4
#define BODY_NEW    8          /* added while a step was running: joins at the next one */

/* a convex part: n vertices (CCW) with outward edge normals; n = 1 is a circle,
   centre v[0], radius nrm[0].x */
typedef struct { u8 n; V2 v[MAX_VERTS]; V2 nrm[MAX_VERTS]; V2 bmin, bmax; } Poly;

/* geometry + state (EWRAM) */
typedef struct {
  V2 p; fx a;            /* position (COM), angle in Q16 radians */
  fx c, s;               /* cos/sin of angle */
  V2 bmin, bmax;         /* body AABB */
  s16 sleepTime;
  u8 flags, shape, color, nparts;
  u8 frx, fry, fra, var;  /* sub-LSB position/angle remainders (Q24 integration); var: shape from phys_add_shape */
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

/* ---- arcade --------------------------------------------------------------
 * Shapes: a piece scaled (size 0 tiny 0.5x, 1 normal, 2 giant 1.5x) and tipped
 * toward the viewer in quarter turns (tip 0 as is, 1 edge-on, 2 upside down,
 * 3 edge-on the other way). Edge-on is drawn and collides as the piece squashed
 * to one cell tall, the thickness of a block seen from the side; it keeps its
 * mass. sx, sy: how the sprite is scaled (sy < 0: flipped). */
typedef struct {
  s32 mass, inertia, top, bottom;     /* Q16, like PieceDef */
  s32 sox, soy, sx, sy;
  u8 shape, size, tip, nparts;
  PiecePart parts[PIECE_MAX_PARTS];
} PhysShape;
void phys_shape(int shape, int size, int tip, PhysShape* out);
int phys_add_shape(const PhysShape* ps, int color, fx x, fx y, fx angle);
int phys_shape_world(const PhysShape* ps, fx x, fx y, fx angle, Poly* polys);
const PhysShape* phys_body_shape(int i);           /* 0 for a plain piece */
fx phys_body_mass(int i);

/* world settings (arcade: lighter gravity, grippier and less bouncy blocks) */
void phys_set_params(fx gravity, fx friction, fx restitution);

/* Magnets: bodies with a charge pull opposite charges and push like ones within
   MAG_RANGE of their outlines, like the web version's (forces scaled to block
   masses, capped at 1.5 g). */
extern s8 phys_charge[MAX_BODIES];

/* Welds (sticky blocks): hold two bodies (or a body and the static base) in
   their current relative pose. Welded bodies don't collide, wake and sleep
   together. Between steps only. Returns 0 if they already are (indirectly). */
int phys_weld(int a, int b);
int phys_weld_count(void);
/* every pair of bodies touching after the last step where flags[a] | flags[b] */
typedef void (*PhysPairFn)(int a, int b);
void phys_touching(const u8* flags, PhysPairFn fn);
/* take a body out and wake whatever rested on or touched it (explosions) */
void phys_remove_wake(int i);
/* runs once after each step, before the next one starts: the game can add
   welds and remove bodies there */
extern void (*phys_after_step)(void);
