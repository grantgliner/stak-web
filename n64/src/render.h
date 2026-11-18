/* Software renderer for STAK! N64.
 *
 * Everything is drawn by the CPU straight into the 320x240 RGBA5551 frame
 * buffer, with a 16-bit depth buffer. The N64's RSP and RDP are not used: the
 * browser emulator the game ships in (EmulatorJS's mupen64plus_next) only has a
 * high-level RSP and its video plugin ignores homebrew RDP triangles, while
 * CPU-drawn frames show up everywhere (and on real hardware). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gen/assets_gen.h"

#define SCR_W 320
#define SCR_H 240

typedef struct { float x, y, z; } vec3;

static inline vec3 v3(float x, float y, float z) { vec3 v = { x, y, z }; return v; }

/* colours are 0xRRGGBB */
static inline uint16_t rgb16(uint32_t c) {
    return (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 11) & 31) << 6) | (((c >> 3) & 31) << 1) | 1);
}

void r_init(void);
void r_begin(uint16_t* fb);          /* start a frame: background + clear depth (only what the last frame in fb touched) */
void r_end(void);                    /* finish it */
void r_invalidate(void);             /* the background changed: next frames copy it whole */
/* draw what rarely changes into the background: r_mesh and d_* calls between these go there */
void r_bake_begin(void);
void r_bake_end(void);
void r_set_sky(uint32_t top, uint32_t mid, uint32_t bottom, int horizon);

/* camera: eye, look-at target, vertical field of view (radians) */
void r_camera(vec3 eye, vec3 target, float fov);
/* project a world point to the screen; returns false when behind the camera */
bool r_project(vec3 p, float* sx, float* sy, float* depth);
/* screen point -> world point on the plane z = plane_z */
vec3 r_unproject_z(float sx, float sy, float plane_z);

/* draw modes */
#define RM_OPAQUE   0
#define RM_GHOST    1      /* 50% translucent, depth-tested, no depth write */
#define RM_NODEPTH  2      /* opaque, drawn over everything */
#define RM_FLASH    4      /* add a white glow */

/* Draw a mesh with a model transform: rotation `rot` (row-major 3x3), scale, then translation.
   mat_colors[m] is the colour of material m (0xRRGGBB). */
void r_mesh(const Mesh* m, const float rot[9], float scale, vec3 pos, const uint32_t* mat_colors, int mode);
void r_floor(float y, float z0, float z1, float x0, float x1, uint32_t near_col, uint32_t far_col);
/* a soft dark ellipse on the floor (blob shadow) */
void r_shadow(vec3 c, float rx, float rz, int strength);

/* rotation helpers (row-major 3x3) */
void rot_z(float a, float out[9]);
void rot_x(float a, float out[9]);
void rot_y(float a, float out[9]);
void rot_mul(const float a[9], const float b[9], float out[9]);

/* ---- 2D ---- */
void d_rect(int x, int y, int w, int h, uint32_t col, int alpha);          /* alpha 0..256 */
void d_round_rect(int x, int y, int w, int h, int r, uint32_t col, int alpha);
void d_circle(int cx, int cy, int r, uint32_t col, int alpha);
int  d_text(const Font* f, int x, int y, const char* s, uint32_t col, int alpha);   /* y = top; returns width */
int  d_text_width(const Font* f, const char* s);
void d_text_c(const Font* f, int cx, int y, const char* s, uint32_t col, int alpha);  /* centred */
void d_logo(int x, int y, int alpha);
/* a controller button glyph: coloured disc with a label */
void d_button(int cx, int cy, int r, uint32_t col, const char* label);

/* sprites: a mesh rendered once (see r_make_sprite), drawn flat with d_sprite */
#define SPRITE_MAX 72
typedef struct { int w, h, hx, hy; uint16_t px[SPRITE_MAX * SPRITE_MAX]; } Sprite;
void r_make_sprite(const Mesh* m, const float rot[9], float scale, const uint32_t* mat_colors, Sprite* out);
void d_sprite(const Sprite* s, int cx, int cy);

extern uint16_t* r_fb;
extern int r_tris_drawn;
