/* STAK! N64: classic mode.
 *
 * Looks and plays like the web version: three blocks lie in a tray in front of
 * the base; a gloved hand (the cursor) picks one up, it swings upright above the
 * tower, you steer it, turn it and let go. The tower runs on the GBA port's 2D
 * physics engine (cells, Q16.16); everything is drawn in 3D by render.c. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "game.h"
#include "render.h"
#include "audio.h"
#include "physics.h"

/* ------------------------------------------------------------------ constants */
#define BASE_HW     FX(4.1)
#define BASE_HH     FX(1.0)
#define HOLD_GAP    FX(0.75)
#define FALL_Y      FX(-4.0)
#define FALL_X      FX(18.0)
#define ROT_STEP    (FX_PI / 4)
#define ROT_FRAMES  6
#define LIFT_FRAMES 18
#define TRAY_Z      5.0f
#define TRAY_TILT   (-PI / 2 + 0.45f)   /* tray blocks lie back, propped toward the camera */
#define HAND_HOVER  1.25f      /* the free glove glides this far above the tray */
#define RELEASE_FRAMES 22
#define CAM_X       (-1.6f)    /* the scene sits right of the score panel */
#define PI          3.14159265f

static const uint32_t CLASSIC_COLS[7] = { 0xFF8A00, 0xFFC700, 0x00FF27, 0x00D1FF, 0x0075FF, 0xAD00FF, 0xFF00E5 };
static const uint8_t pool_classic[] = { 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const float tray_x[3] = { -4.7f, 0.0f, 4.7f };
#include "messages.inc"

/* button colours, as on the controller */
#define COL_A     0x3A5BE8
#define COL_B     0x1FA34A
#define COL_C     0xF2C511
#define COL_START 0xE0322E
#define COL_GRAY  0x8C8F9C

enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER };
enum { H_FREE, H_LIFT, H_HELD, H_RETURN, H_RELEASE };

typedef struct { uint8_t shape, color; float pop; } Slot;   /* pop: 0..1 grow-in after a refill */

static int state;
static uint32_t frame, rng = 0x2545F491;
static SaveData save;
static Slot tray[3];
static float hover_amt[3];
static int hover = -1;               /* tray slot under the glove */
static int slot;                     /* slot of the held piece */
static int hand_state, hand_t;
static float hand_x, hand_z = TRAY_Z;   /* the free glove, over the tray */
static float hand_tilt;
static vec3 hand_from;                  /* where the glove was when a grab or release began */
static fx hx, hy, hangle, hangle_from, hangle_to;
static int hrot_t, hcolliding, move_frames;
static int blocks, score, new_best;
static float look_y = 0.6f, yaw, pitch = 0.40f, yaw_v;
static int over_t;
static int pop_t, pop_val; static float pop_x, pop_y;
static int err_t;

/* ------------------------------------------------------------------ helpers */
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float ease(float t) { if (t <= 0) return 0; if (t >= 1) return 1; return t * t * (3 - 2 * t); }
static float fxf(fx v) { return v / 65536.0f; }

static Slot new_slot(int idx) {
    Slot s;
    for (;;) {
        s.shape = pool_classic[(rnd() >> 8) % sizeof pool_classic];
        int dup = 0;
        for (int k = 0; k < 3; k++) if (k != idx && tray[k].shape == s.shape) dup = 1;
        if (!dup) break;
    }
    for (;;) {
        s.color = (rnd() >> 8) % 7;
        int dup = 0;
        for (int k = 0; k < 3; k++) if (k != idx && tray[k].color == s.color) dup = 1;
        if (!dup) break;
    }
    s.pop = 0;
    return s;
}

static fx tower_top(void) {
    fx top = 0;
    for (int i = 0; i < phys_count; i++) {
        Body* b = &phys_bodies[i];
        if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
        if (b->bmax.y > top) top = b->bmax.y;
    }
    return top;
}

static float tray_y(void) { float y = look_y - 5.6f; return y < -1.55f ? -1.55f : y; }

static vec3 tray_pos(int k) {
    float h = hover_amt[k];
    return v3(tray_x[k], tray_y() + h * (0.45f + 0.12f * sinf(frame * 0.15f)), TRAY_Z);
}

static void held_spawn_pose(void) {
    hx = 0;
    hy = tower_top() + piece_defs[tray[slot].shape].bottom + HOLD_GAP;
    hangle = hangle_from = hangle_to = 0;
    hrot_t = ROT_FRAMES;
}

static void held_update_collision(void) {
    Poly polys[PIECE_MAX_PARTS];
    int n = phys_piece_world(tray[slot].shape, hx, hy, hangle, polys);
    hcolliding = phys_overlaps(polys, n, FX(0.02));
}

/* ------------------------------------------------------------------ states */
static void make_hand_sprites(void);
static void title_enter(void) {
    state = ST_TITLE;
    phys_init();
    hand_state = H_FREE;
    look_y = 4.4f;
}

static void play_enter(void) {
    state = ST_PLAY;
    phys_init();
    phys_add_static_box(0, -BASE_HH, BASE_HW, BASE_HH);
    tray[0].shape = tray[1].shape = tray[2].shape = 0xFF;
    tray[0].color = tray[1].color = tray[2].color = 0xFF;
    for (int k = 0; k < 3; k++) { tray[k] = new_slot(k); tray[k].pop = -0.25f * k; hover_amt[k] = 0; }
    blocks = score = new_best = 0;
    hand_state = H_FREE; hover = -1;
    hand_x = 0; hand_z = TRAY_Z + 0.6f;
    pop_t = 0; err_t = 0;
}

static void game_over(void) {
    state = ST_OVER; over_t = 0;
    if (hand_state != H_FREE) { hand_state = H_FREE; }
    if (blocks > save.best_blocks) { save.best_blocks = blocks; new_best = 1; }
    if ((uint32_t)score > save.best_score) save.best_score = score;
    plat_save(&save);
    snd_sfx(SFX_GAMEOVER);
}

void game_init(void) {
    r_init();
    make_hand_sprites();
    plat_load(&save);
    if (save.magic != 0x53544B36) { memset(&save, 0, sizeof save); save.magic = 0x53544B36; }
    title_enter();
}

/* ------------------------------------------------------------------ play */
static void place_held(void) {
    int b = phys_add_piece(tray[slot].shape, tray[slot].color, hx, hy, hangle_to);
    if (b < 0) { snd_sfx(SFX_ERROR); return; }
    blocks++;
    score += 250 * blocks;
    pop_t = 50; pop_val = 250 * blocks; pop_x = fxf(hx); pop_y = fxf(hy + piece_defs[tray[slot].shape].top);
    tray[slot] = new_slot(slot);
    hand_state = H_RELEASE; hand_t = 0;
    snd_sfx(SFX_PLACE);
}

static vec3 free_hand_pos(void) { return v3(hand_x, tray_y() + HAND_HOVER + 0.1f * sinf(frame * 0.09f), hand_z); }

static void move_cursor(const Input* in) {
    /* the stick slides the glove over the tray: fine near the centre, quick at the rim */
    float ax = fabsf(in->sx), ay = fabsf(in->sy);
    float vx = (in->sx < 0 ? -1 : 1) * powf(ax, 1.5f) * 0.24f;
    float vz = (in->sy < 0 ? -1 : 1) * powf(ay, 1.5f) * 0.16f;
    hand_x += vx; hand_z -= vz;
    /* D-pad hops between the tray blocks */
    if (in->down & (BTN_DL | BTN_DR)) {
        int k = hover >= 0 ? hover + ((in->down & BTN_DR) ? 1 : -1) : (hand_x < tray_x[0] ? 0 : hand_x > tray_x[2] ? 2 : 1);
        if (k < 0) k = 0; if (k > 2) k = 2;
        hand_x = tray_x[k]; hand_z = TRAY_Z + 0.3f;
    }
    if (hand_x < -7.0f) hand_x = -7.0f; if (hand_x > 7.0f) hand_x = 7.0f;
    if (hand_z < TRAY_Z - 1.6f) hand_z = TRAY_Z - 1.6f; if (hand_z > TRAY_Z + 2.2f) hand_z = TRAY_Z + 2.2f;
    hand_tilt += (-vx * 0.7f - hand_tilt) * 0.2f;
}

static void find_hover(void) {
    int best = -1; float bd = 2.2f * 2.2f;
    for (int k = 0; k < 3; k++) {
        if (tray[k].pop < 1) continue;
        float dx = tray_x[k] - hand_x, dz = (TRAY_Z + 0.3f - hand_z) * 1.5f;
        float q = dx * dx + dz * dz;
        if (q < bd) { bd = q; best = k; }
    }
    if (best != hover && best >= 0) snd_sfx(SFX_SELECT);
    hover = best;
}

static void play_input(const Input* in) {
    if (in->down & BTN_START) { state = ST_PAUSE; snd_sfx(SFX_SELECT); return; }
    switch (hand_state) {
    case H_RELEASE:
        if (++hand_t >= RELEASE_FRAMES) hand_state = H_FREE;
        /* fall through: the glove can already move */
    case H_FREE:
        if (hand_state == H_FREE) {
            move_cursor(in);
            find_hover();
            if (in->down & BTN_A) {
                if (hover >= 0) {
                    slot = hover; hand_state = H_LIFT; hand_t = 0; hand_from = free_hand_pos();
                    held_spawn_pose();
                    move_frames = 0;
                    snd_sfx(SFX_PICKUP);
                } else snd_sfx(SFX_ERROR);
            }
        }
        break;
    case H_LIFT:
        if (++hand_t >= LIFT_FRAMES) { hand_state = H_HELD; held_update_collision(); }
        break;
    case H_RETURN:
        if (++hand_t >= LIFT_FRAMES) {
            hand_state = H_FREE;
            hand_x = tray_x[slot]; hand_z = TRAY_Z + 0.3f;
        }
        break;
    case H_HELD: {
        /* move: analog stick, or the D-pad with the GBA's acceleration */
        float ax = fabsf(in->sx), ay = fabsf(in->sy);
        float vx = (in->sx < 0 ? -1 : 1) * powf(ax, 1.6f) * 0.30f;
        float vy = (in->sy < 0 ? -1 : 1) * powf(ay, 1.6f) * 0.30f;
        int dx = ((in->held & BTN_DR) ? 1 : 0) - ((in->held & BTN_DL) ? 1 : 0);
        int dy = ((in->held & BTN_DU) ? 1 : 0) - ((in->held & BTN_DD) ? 1 : 0);
        if (dx || dy) {
            move_frames++;
            float sp = 0.05f + move_frames * 0.008f; if (sp > 0.22f) sp = 0.22f;
            vx += dx * sp; vy += dy * sp;
        } else move_frames = 0;
        hx += FX(vx); hy += FX(vy);
        if (hx < FX(-7.5)) hx = FX(-7.5); if (hx > FX(7.5)) hx = FX(7.5);
        if (hy < FX(-1.5)) hy = FX(-1.5); if (hy > FX(90)) hy = FX(90);
        hand_tilt += (-vx * 0.8f - hand_tilt) * 0.2f;
        /* rotate 45 degrees: Z or L counter-clockwise, R clockwise */
        if (in->down & (BTN_Z | BTN_L | BTN_R)) {
            int ccw = (in->down & (BTN_Z | BTN_L)) != 0;
            hangle_from = hangle; hangle_to += ccw ? ROT_STEP : -ROT_STEP;
            hrot_t = 0;
            snd_sfx(ccw ? SFX_ROTATE_L : SFX_ROTATE_R);
        }
        if (hrot_t < ROT_FRAMES) {
            hrot_t++;
            hangle = hangle_from + (fx)(((int64_t)(hangle_to - hangle_from) * hrot_t) / ROT_FRAMES);
        } else {
            while (hangle_to > FX_PI) hangle_to -= 2 * FX_PI;
            while (hangle_to < -FX_PI) hangle_to += 2 * FX_PI;
            hangle = hangle_from = hangle_to;
        }
        held_update_collision();
        if (in->down & BTN_B) { hand_state = H_RETURN; hand_t = 0; snd_sfx(SFX_PUTBACK); break; }
        if (in->down & BTN_A) {
            if (hcolliding || hrot_t < ROT_FRAMES) { snd_sfx(SFX_ERROR); err_t = 12; break; }
            place_held();
            hand_x = fxf(hx); if (hand_x < -7.0f) hand_x = -7.0f; if (hand_x > 7.0f) hand_x = 7.0f;
            hand_z = TRAY_Z + 0.6f;
        }
        break;
    }
    }
}

static void check_fall(void) {
    for (int i = 0; i < phys_count; i++) {
        Body* b = &phys_bodies[i];
        if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
        if (b->p.y < FALL_Y || b->p.x > FALL_X || b->p.x < -FALL_X) { game_over(); return; }
    }
}

static void drop_fallen(void) {
    for (int i = 0; i < phys_count; i++) {
        Body* b = &phys_bodies[i];
        if ((b->flags & (BODY_USED | BODY_STATIC)) == BODY_USED && b->p.y < FALL_Y - FX(40.0)) phys_remove(i);
    }
}

static void update_camera(const Input* in) {
    /* C buttons swing the camera round the tower a little */
    if (in->held & BTN_CL) yaw_v -= 0.004f;
    if (in->held & BTN_CR) yaw_v += 0.004f;
    yaw_v *= 0.85f; yaw += yaw_v;
    if (yaw < -0.55f) { yaw = -0.55f; yaw_v = 0; } if (yaw > 0.55f) { yaw = 0.55f; yaw_v = 0; }
    if (in->held & BTN_CU) pitch += 0.012f;
    if (in->held & BTN_CD) pitch -= 0.012f;
    if (pitch < 0.04f) pitch = 0.04f; if (pitch > 0.6f) pitch = 0.6f;
    float target;
    if (state == ST_TITLE) target = 4.4f;
    else if (state == ST_OVER) target = 0.6f;
    else {
        float top = fxf(tower_top());
        target = top - 1.0f;
        if (hand_state == H_HELD || hand_state == H_LIFT) {
            float h = fxf(hy);
            if (h - 4.0f > target) target = h - 4.0f;
        }
        if (target < 0.6f) target = 0.6f;
    }
    float dl = (target - look_y) * (state == ST_OVER ? 0.05f : 0.08f);
    if (fabsf(target - look_y) < 0.004f) look_y = target; else look_y += dl;
}

static void set_camera(void) {
    float d = state == ST_TITLE ? 33.0f : 19.0f;
    float cy = look_y;
    float cx = state == ST_TITLE ? 0 : CAM_X;
    vec3 eye = v3(cx + sinf(yaw) * cosf(pitch) * d, cy + sinf(pitch) * d, cosf(yaw) * cosf(pitch) * d + 1.5f);
    r_camera(eye, v3(cx, cy, 1.5f), 0.78f);
}

void game_update(const Input* in, int steps) {
    frame++;
    for (int k = 0; k < 3; k++) {
        if (tray[k].pop < 1) { tray[k].pop += 0.06f; if (tray[k].pop > 1) tray[k].pop = 1; }
        float want = (hover == k && hand_state == H_FREE && state == ST_PLAY) ? 1.0f : 0.0f;
        hover_amt[k] += (want - hover_amt[k]) * 0.25f;
    }
    if (pop_t) pop_t--;
    if (err_t) err_t--;
    set_camera();          /* input needs the current projection */
    switch (state) {
    case ST_TITLE:
        if (in->down & (BTN_START | BTN_A)) { snd_sfx(SFX_SELECT); yaw = 0; play_enter(); }
        break;
    case ST_PLAY:
        play_input(in);
        if (state != ST_PLAY) break;
        for (int s = 0; s < steps; s++) phys_step();
        check_fall();
        break;
    case ST_PAUSE:
        if (in->down & BTN_START) { state = ST_PLAY; snd_sfx(SFX_SELECT); }
        else if (in->down & BTN_B) { snd_sfx(SFX_PUTBACK); title_enter(); }
        break;
    case ST_OVER:
        over_t++;
        for (int s = 0; s < steps; s++) phys_step();
        drop_fallen();
        if (over_t > 90) {
            if (in->down & BTN_A) { snd_sfx(SFX_SELECT); play_enter(); }
            else if (in->down & (BTN_B | BTN_START)) { snd_sfx(SFX_PUTBACK); title_enter(); }
        }
        break;
    }
    if (state != ST_TITLE) update_camera(in);
    else update_camera(&(Input){ 0 });
}

/* ------------------------------------------------------------------ drawing */
static void draw_title_static(void);
static void draw_panel(void);
static const float I3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void draw_base(void) {
    uint32_t c[4] = { 0x6E727C, 0xE8ECF4, 0, 0 };
    r_mesh(&meshes[MESH_BASE], I3, 1.0f, v3(0, 0, 0), c, RM_OPAQUE);
}

static void draw_piece(int shape, int color, const float R[9], float sc, vec3 p, int mode) {
    uint32_t c[1] = { color < 7 ? CLASSIC_COLS[color] : (uint32_t)color };
    r_mesh(&meshes[MESH_PIECE0 + shape], R, sc, p, c, mode);
}

/* which: 1 sleeping bodies, 2 awake ones, 3 all */
static void draw_bodies(int which) {
    for (int i = 0; i < phys_count; i++) {
        Body* b = &phys_bodies[i];
        if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
        if (!(which & ((b->flags & BODY_SLEEP) ? 1 : 2))) continue;
        float R[9]; rot_z(fxf(b->a), R);
        draw_piece(b->shape, b->color, R, 1.0f, v3(fxf(b->p.x), fxf(b->p.y), 0), RM_OPAQUE);
    }
}

/* pose of the held piece: between the tray (lying flat) and upright above the
   tower, t = 0..1 */
static void held_pose(float t, float R[9], vec3* p) {
    float e = ease(t);
    vec3 a = tray_pos(slot), b = v3(fxf(hx), fxf(hy), 0);
    p->x = a.x + (b.x - a.x) * e;
    p->y = a.y + (b.y - a.y) * e + sinf(e * PI) * 2.2f;
    p->z = a.z + (b.z - a.z) * e;
    float Rx[9], Rz[9];
    rot_x(TRAY_TILT * (1 - e), Rx);
    rot_z(fxf(hangle) * e, Rz);
    rot_mul(Rz, Rx, R);
}

/* the glove: palm at p, turned by R (back of the hand along local +z, fingers +y) */
/* The glove is a sprite (open and grabbing), made from its model at start-up.
   R and sc are kept for the call sites' sake; only the position counts. */
static Sprite hand_spr[2];
static void make_hand_sprites(void) {
    static const uint32_t hc[4] = { 0xF6F6F2, 0x2A55E0, 0xFFC700, 0xC6C8D2 };
    float Rx[9], Rz[9], R[9];
    rot_x(-0.25f, Rx); rot_z(0.18f, Rz); rot_mul(Rz, Rx, R);
    r_make_sprite(&meshes[MESH_HAND_OPEN], R, 1.25f, hc, &hand_spr[0]);
    r_make_sprite(&meshes[MESH_HAND_GRAB], R, 1.15f, hc, &hand_spr[1]);
}
static void draw_hand(vec3 p, int grab, const float R[9], float sc) {
    (void)R; (void)sc;
    float sx, sy, d;
    if (!r_project(p, &sx, &sy, &d)) return;
    d_sprite(&hand_spr[grab ? 1 : 0], (int)sx, (int)sy + (grab ? 12 : 0));   /* the fist grips the lower edge */
}

/* glove turn for a pose between lying flat (palm down over the tray, e = 0) and upright (e = 1) */
static void hand_rot(float e, float tilt, float R[9]) {
    float Rx[9], Rz[9];
    rot_x(-PI / 2 * 0.82f * (1 - e) - 0.12f * e, Rx);
    rot_z(tilt, Rz);
    rot_mul(Rz, Rx, R);
}

/* where the glove grips a piece posed by (R, p): just off its face, a little low */
static float grip_drop = 0.3f;     /* how far below the centre the glove grips the held piece */
static vec3 grip_pos(const float R[9], vec3 p) {
    const float o[3] = { 0, -grip_drop, 0.95f };
    return v3(p.x + R[0] * o[0] + R[1] * o[1] + R[2] * o[2], p.y + R[3] * o[0] + R[4] * o[1] + R[5] * o[2], p.z + R[6] * o[0] + R[7] * o[1] + R[8] * o[2]);
}

static vec3 vlerp(vec3 a, vec3 b, float t) { return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }

static void draw_tray(void) {
    float R[9]; rot_x(TRAY_TILT, R);
    for (int k = 0; k < 3; k++) {
        if (tray[k].pop <= 0) continue;
        if (k == slot && hand_state != H_FREE && hand_state != H_RELEASE && state == ST_PLAY) continue;
        float s = ease(tray[k].pop);
        s = s * (1 + 0.25f * sinf(s * PI));                  /* a little overshoot */
        int mode = hover_amt[k] > 0.5f ? RM_FLASH : RM_OPAQUE;
        draw_piece(tray[k].shape, tray[k].color, R, s, tray_pos(k), mode);
    }
}

static void draw_held_and_hand(void) {
    if (state != ST_PLAY && state != ST_PAUSE) return;
    float R[9]; vec3 p;
    grip_drop = 0.3f + 0.8f * fxf(piece_defs[tray[slot].shape].bottom);
    switch (hand_state) {
    case H_FREE: {
        float wob = hover >= 0 ? 0.06f * sinf(frame * 0.2f) : 0;
        hand_rot(0, 0.12f + hand_tilt + wob, R);
        draw_hand(free_hand_pos(), 0, R, 1.5f);
        break;
    }
    case H_RELEASE: {
        /* let go, hold still a moment, then drift back down to the tray */
        float Rp[9]; rot_z(fxf(hangle_to), Rp);
        vec3 at = v3(fxf(hx), fxf(hy) - 0.3f, 0.95f);
        float t = ease((hand_t - 6) / (float)(RELEASE_FRAMES - 6));
        hand_rot(t > 0 ? 1 - t : 1, hand_tilt * t, R);
        draw_hand(vlerp(at, free_hand_pos(), t), 0, R, 1.35f + 0.15f * t);
        break;
    }
    case H_LIFT: case H_RETURN: {
        float t = (float)hand_t / LIFT_FRAMES;
        if (hand_state == H_RETURN) t = 1 - t;
        held_pose(t, R, &p);
        draw_piece(tray[slot].shape, tray[slot].color, R, 1.0f, p, RM_OPAQUE);
        float e = ease(t);
        float Rh[9]; hand_rot(e, hand_tilt * e, Rh);
        vec3 g = grip_pos(Rh, p);
        if (hand_state == H_LIFT && hand_t < 5) g = vlerp(hand_from, g, hand_t / 5.0f);
        draw_hand(g, 1, Rh, 1.1f);
        break;
    }
    case H_HELD: {
        held_pose(1, R, &p);
        int bad = hcolliding || err_t;
        if (bad) draw_piece(tray[slot].shape, 0xFF3048, R, 1.0f, p, RM_GHOST);
        else draw_piece(tray[slot].shape, tray[slot].color, R, 1.0f, p, RM_OPAQUE);
        float Rh[9]; hand_rot(1, hand_tilt, Rh);
        draw_hand(grip_pos(Rh, p), 1, Rh, 1.1f);
        break;
    }
    }
}

static void fmt_int(char* b, int v) {
    /* 12,345 */
    char t[16]; int n = 0;
    if (v == 0) t[n++] = '0';
    int g = 0;
    while (v > 0) { if (g == 3) { t[n++] = ','; g = 0; } t[n++] = '0' + v % 10; v /= 10; g++; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0;
}

static void draw_panel(void) {
    char b[32], c[48];
    d_round_rect(10, 10, 96, 104, 12, 0x161C3E, 175);
    d_round_rect(11, 11, 94, 1, 0, 0x8090D0, 90);
    fmt_int(b, blocks);
    d_text_c(&font_big, 58, 12, b, 0xFFFFFF, 256);
    d_text_c(&font_med, 58, 56, "Stacked", 0xFFFFFF, 256);
    fmt_int(b, score); snprintf(c, sizeof c, "%s points", b);
    d_text_c(&font_small, 58, 76, c, 0xAAB4D8, 256);
    if (save.best_blocks) {
        snprintf(c, sizeof c, "Best %d", save.best_blocks);
        d_round_rect(28, 92, 60, 15, 7, 0x0C1030, 160);
        d_text_c(&font_small, 58, 94, c, 0xC8D0F0, 256);
    } else {
        d_round_rect(20, 92, 76, 15, 7, 0x0C1030, 160);
        d_text_c(&font_small, 58, 94, "No high score set", 0xC8D0F0, 256);
    }
}

/* a row of controller hints along the bottom: glyph + label pairs */
typedef struct { uint32_t col; const char* glyph; const char* label; const char* glyph2; } Hint;
static void draw_hints(const Hint* h, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += (h[i].glyph2 ? 37 : h[i].glyph[0] ? 20 : 0) + d_text_width(&font_small, h[i].label) + 12;
    w -= 12;
    int x = SCR_W / 2 - w / 2, y = SCR_H - 20;
    d_round_rect(x - 10, y - 9, w + 20, 19, 9, 0x0C1030, 140);
    for (int i = 0; i < n; i++) {
        if (h[i].glyph2) { d_button(x + 8, y, 7, h[i].col, h[i].glyph); x += 17; d_button(x + 8, y, 7, h[i].col, h[i].glyph2); x += 20; }
        else if (h[i].glyph[0]) { d_button(x + 8, y, 7, h[i].col, h[i].glyph); x += 20; }
        d_text(&font_small, x, y - 7, h[i].label, 0xE6EAF8, 256);
        x += d_text_width(&font_small, h[i].label) + 12;
    }
}

static void draw_popup(void) {
    if (!pop_t) return;
    float sx, sy, d;
    float rise = (50 - pop_t) * 0.04f;
    if (!r_project(v3(pop_x, pop_y + 0.6f + rise, 0.6f), &sx, &sy, &d)) return;
    char b[16]; b[0] = '+'; fmt_int(b + 1, pop_val);
    int a = pop_t > 20 ? 256 : pop_t * 12;
    d_text_c(&font_title, (int)sx + 1, (int)sy - 10, b, 0x101430, a / 2);
    d_text_c(&font_title, (int)sx, (int)sy - 11, b, 0xFFFFFF, a);
}

static void wrap_center(const Font* f, int cx, int y, int maxw, const char* s, uint32_t col) {
    char line[96]; int n = 0; const char* p = s;
    while (*p) {
        /* take words while they fit */
        const char* q = p; int last = -1; n = 0;
        while (*q && n < 95) {
            line[n] = *q; line[n + 1] = 0;
            if (*q == ' ') last = n;
            if (d_text_width(f, line) > maxw && last > 0) { n = last; q = p + last + 1; break; }
            n++; q++;
        }
        line[n] = 0;
        d_text_c(f, cx, y, line, col, 256);
        y += f->line;
        p = q;
    }
}

static void draw_title_static(void) {
    d_logo(SCR_W / 2 - LOGO_W / 2, 14, 256);
    if (save.best_blocks) {
        char c[32]; snprintf(c, sizeof c, "Best: %d stacked", save.best_blocks);
        d_text_c(&font_small, SCR_W / 2, 226, c, 0xAAB4D8, 256);
    }
}

static void draw_title(void) {
    if ((frame / 24) & 1 || frame < 30) {
        d_button(SCR_W / 2 - 40, 210, 8, COL_START, "S");
        d_text(&font_med, SCR_W / 2 - 28, 202, "Press START", 0xFFFFFF, 256);
    }
}

static void draw_over_panel(int a) {
    int pw = 220, ph = 136, px = SCR_W / 2 - pw / 2, py = 44;
    d_round_rect(px, py, pw, ph, 14, 0x161C3E, a * 238 / 256);
    d_text_c(&font_title, SCR_W / 2, py + 10, "Game over", 0xFFD84A, a);
    char b[24]; fmt_int(b, blocks);
    d_text_c(&font_big, SCR_W / 2, py + 34, b, 0xFFFFFF, a);
    d_text_c(&font_small, SCR_W / 2, py + 76, blocks == 1 ? "block stacked" : "blocks stacked", 0xAAB4D8, a);
    const char* msg = blocks < 50 ? messages[blocks] : blocks >= 100 ? "Stacking among the stars." : "Masterful.";
    if (a == 256) wrap_center(&font_small, SCR_W / 2, py + 92, pw - 30, msg, 0xE6EAF8);
}

/* the panel fades in live, then lives in the background (see bake) */
static void draw_over_static(void);
static void draw_over(int scene_baked) {
    if (over_t >= 50 && over_t < 70) draw_over_panel((over_t - 50) * 256 / 20);
    else if (!scene_baked) draw_over_static();     /* live 3D would cover the baked panel */
    if (new_best && over_t >= 70 && ((frame / 16) & 1)) d_text_c(&font_med, SCR_W / 2, 44 - 20, "New best!", 0xFFD84A, 256);
}

static void draw_over_static(void) {
    if (over_t < 70) return;
    draw_over_panel(256);
    if (over_t > 90) {
        Hint h[2] = { { COL_A, "A", "Play again" }, { COL_B, "B", "Title" } };
        draw_hints(h, 2);
    }
}

static void draw_pause(void) {
    d_round_rect(SCR_W / 2 - 70, 80, 140, 70, 12, 0x161C3E, 220);
    d_text_c(&font_title, SCR_W / 2, 90, "Paused", 0xFFFFFF, 256);
    Hint h[2] = { { COL_START, "S", "Resume" }, { COL_B, "B", "Quit" } };
    draw_hints(h, 2);
}

static int hint_id(void) {
    if (state != ST_PLAY) return 0;
    if (hand_state == H_HELD) return 1;
    if (hand_state == H_FREE && hover >= 0) return 2;
    if (blocks == 0 && hand_state == H_FREE) return 3;
    return 0;
}

static void draw_hint_bar(int id) {
    if (id == 1) {
        Hint h[3] = { { COL_A, "A", "Drop" }, { COL_GRAY, "Z", "Rotate", "R" }, { COL_B, "B", "Put back" } };
        draw_hints(h, 3);
    } else if (id == 2) {
        Hint h[1] = { { COL_A, "A", "Grab" } };
        draw_hints(h, 1);
    } else if (id == 3) {
        Hint h[1] = { { COL_GRAY, "", "Point at a block with the stick" } };
        draw_hints(h, 1);
    }
}

/* What rarely changes is drawn into the background once: the HUD always, and
   the base and settled blocks while the camera is still (they are "the scene").
   While the camera moves or blocks wake up, only the HUD is baked and the scene
   is drawn every frame. */
static uint32_t hud_key(void) {
    if (state == ST_TITLE) return 0xFFFFFF00u | (save.best_blocks & 0xFF);
    int over = state != ST_OVER ? 0 : over_t > 90 ? 3 : over_t >= 70 ? 2 : 1;
    return (uint32_t)blocks << 20 ^ (uint32_t)score << 4 ^ (uint32_t)hint_id() ^ (uint32_t)save.best_blocks << 12 ^ (uint32_t)over << 29;
}

static uint32_t scene_key(void) {
    uint32_t h = 2166136261u;
#define MIX(v) h = (h ^ (uint32_t)(v)) * 16777619u
    MIX((int)(look_y * 1024)); MIX((int)(yaw * 4096)); MIX((int)(pitch * 4096)); MIX(state == ST_TITLE);
    for (int i = 0; i < phys_count; i++) {
        Body* b = &phys_bodies[i];
        if ((b->flags & (BODY_USED | BODY_STATIC | BODY_SLEEP)) != (BODY_USED | BODY_SLEEP)) continue;
        MIX(i); MIX(b->p.x); MIX(b->p.y); MIX(b->a);
    }
#undef MIX
    return h;
}

static const int title_sh[1] = { 9 };
static void draw_title_tower(void) {
    float y = 0;
    for (int k = 0; k < 1; k++) {
        const PieceDef* pd = &piece_defs[title_sh[k]];
        float R[9]; rot_z(0, R);
        float bot = pd->bottom / 65536.0f, top = pd->top / 65536.0f;
        draw_piece(title_sh[k], k * 3 + 1, R, 1.0f, v3(k ? -0.7f : 0, y + bot, 0), RM_OPAQUE);
        y += bot + top;
    }
}
static float title_tower_top(void) {
    float y = 0;
    for (int k = 0; k < 1; k++) y += (piece_defs[title_sh[k]].bottom + piece_defs[title_sh[k]].top) / 65536.0f;
    return y;
}

static uint32_t baked_hud = 1, baked_scene = 1, last_scene;   /* baked_scene 0: the scene is not baked */
static int scene_still;

/* returns whether the scene is in the background */
static int bake(void) {
    uint32_t hk = hud_key(), sk = scene_key();
    scene_still = sk == last_scene ? scene_still + 1 : 0;
    last_scene = sk;
    uint32_t want = scene_still >= 2 ? (sk | 1) : 0;
    if (hk == baked_hud && want == baked_scene) return baked_scene != 0;
    r_bake_begin();
    if (want) {
        draw_base();
        if (state == ST_TITLE) draw_title_tower();
        else draw_bodies(1);
    }
    if (state == ST_TITLE) draw_title_static();
    else { if (state != ST_OVER) draw_panel(); draw_hint_bar(hint_id()); if (state == ST_OVER && want) draw_over_static(); }
    r_bake_end();
    baked_hud = hk; baked_scene = want;
    return want != 0;
}

void (*game_prof)(int) = 0;
#define PROF(k) do { if (game_prof) game_prof(k); } while (0)

void game_draw(uint16_t* fb) {
    PROF(0);
    set_camera();
    int baked = bake();
    r_begin(fb);
    PROF(1);
    if (!baked) draw_base();
    PROF(2);
    if (state == ST_TITLE) {
        if (!baked) draw_title_tower();
        /* the glove bringing the next block */
        float R[9]; rot_z(0.3f * sinf(frame * 0.03f), R);
        vec3 hp = v3(1.2f * sinf(frame * 0.021f), title_tower_top() + 3.2f + 0.25f * sinf(frame * 0.05f), 0);
        draw_piece(0, 5, R, 1.0f, hp, RM_OPAQUE);
        float Rh[9]; hand_rot(1, 0.1f * sinf(frame * 0.03f), Rh);
        draw_hand(v3(hp.x, hp.y - 1.7f, 1.0f), 1, Rh, 1.35f);
        draw_title();
        r_end();
        return;
    }
    draw_bodies(baked ? 2 : 3);
    if (state != ST_OVER) draw_tray();
    PROF(3);
    draw_held_and_hand();
    PROF(4);
    draw_popup();
    PROF(5);
    if (state == ST_OVER) draw_over(baked);
    if (state == ST_PAUSE) draw_pause();
    PROF(6);
    r_end();
}
