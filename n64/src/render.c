#include <string.h>
#include <math.h>
#include "render.h"

uint16_t* r_fb;
int r_tris_drawn;
int r_px, r_spans, r_tests;
static uint16_t zbuf_mem[SCR_W * SCR_H] __attribute__((aligned(16)));
static uint16_t zstatic[SCR_W * SCR_H] __attribute__((aligned(16)));   /* depth of the baked background */
static uint16_t* zbuf = zbuf_mem;
static uint16_t sky[SCR_W * SCR_H] __attribute__((aligned(16)));
static uint16_t sky_clean[SCR_W * SCR_H] __attribute__((aligned(16)));

/* region of the frame drawn this frame (see r_begin) */
int r_dx0, r_dy0, r_dx1, r_dy1;
static inline void dirty(int x0, int y0, int x1, int y1) {
    if (x0 < r_dx0) r_dx0 = x0; if (y0 < r_dy0) r_dy0 = y0;
    if (x1 > r_dx1) r_dx1 = x1; if (y1 > r_dy1) r_dy1 = y1;
}

/* camera */
static float V[12];          /* view matrix rows (3x4) */
static float focal;          /* pixels per unit at depth 1 */
static vec3 cam_eye;
#define NEAR 0.5f

/* light (world space, normalised): from the upper left front */
static vec3 L = { -0.42f, 0.78f, 0.46f };
static vec3 Hh;              /* half vector with the view direction (for gloss) */

void rot_z(float a, float o[9]) { float c = cosf(a), s = sinf(a); float r[9] = { c, -s, 0, s, c, 0, 0, 0, 1 }; memcpy(o, r, sizeof r); }
void rot_x(float a, float o[9]) { float c = cosf(a), s = sinf(a); float r[9] = { 1, 0, 0, 0, c, -s, 0, s, c }; memcpy(o, r, sizeof r); }
void rot_y(float a, float o[9]) { float c = cosf(a), s = sinf(a); float r[9] = { c, 0, s, 0, 1, 0, -s, 0, c }; memcpy(o, r, sizeof r); }
void rot_mul(const float a[9], const float b[9], float o[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, r, sizeof r);
}

static vec3 vnorm(vec3 v) { float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); if (l < 1e-9f) return v; return v3(v.x / l, v.y / l, v.z / l); }
static vec3 vcross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void r_init(void) {
    r_set_sky(0x0B0F24, 0x1A2350, 0x2B3A7A, 150);
}

void r_invalidate(void);
void r_set_sky(uint32_t top, uint32_t mid, uint32_t bot, int horizon) {
    r_invalidate();
    /* vertical gradient, ordered-dithered to soften 15-bit banding */
    static const int8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = 0; y < SCR_H; y++) {
        uint32_t a, b; float t;
        if (y < horizon) { a = top; b = mid; t = (float)y / horizon; }
        else { a = mid; b = bot; t = (float)(y - horizon) / (SCR_H - horizon); }
        float r = ((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t;
        float g = ((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t;
        float bl = (a & 255) * (1 - t) + (b & 255) * t;
        for (int x = 0; x < SCR_W; x++) {
            int d = bayer[y & 3][x & 3] / 2 - 4;   /* -4..3 in 8-bit units */
            int ri = (int)r + d, gi = (int)g + d, bi = (int)bl + d;
            if (ri < 0) ri = 0; if (gi < 0) gi = 0; if (bi < 0) bi = 0;
            if (ri > 255) ri = 255; if (gi > 255) gi = 255; if (bi > 255) bi = 255;
            sky[y * SCR_W + x] = sky_clean[y * SCR_W + x] = rgb16((ri << 16) | (gi << 8) | bi);
        }
    }
}

/* Each frame buffer remembers the region drawn into it last time; the next
   frame in that buffer restores only that much of the background, and the depth
   buffer is cleared only where the last frame drew. r_invalidate() forces full
   copies (after the background changes). */
#define NBUF 3
static uint16_t* buf_ptr[NBUF];
static int buf_rect[NBUF][4], buf_full[NBUF];
static int z_rect[4] = { 0, 0, SCR_W, SCR_H };

void r_invalidate(void) { for (int i = 0; i < NBUF; i++) buf_full[i] = 1; }

void r_begin(uint16_t* fb) {
    r_fb = fb;
    int k;
    for (k = 0; k < NBUF; k++) if (buf_ptr[k] == fb) break;
    if (k == NBUF) {
        for (k = 0; k < NBUF; k++) if (!buf_ptr[k]) break;
        if (k == NBUF) k = 0;
        buf_ptr[k] = fb; buf_full[k] = 1;
    }
    if (buf_full[k]) {
        memcpy(fb, sky, sizeof sky);
        buf_full[k] = 0;
    } else {
        int x0 = buf_rect[k][0] & ~3, x1 = (buf_rect[k][2] + 3) & ~3, y0 = buf_rect[k][1], y1 = buf_rect[k][3];
        if (x1 > SCR_W) x1 = SCR_W;
        for (int y = y0; y < y1; y++) memcpy(fb + y * SCR_W + x0, sky + y * SCR_W + x0, (x1 - x0) * 2);
    }
    {
        int x0 = z_rect[0] & ~3, x1 = (z_rect[2] + 3) & ~3, y0 = z_rect[1], y1 = z_rect[3];
        if (x1 > SCR_W) x1 = SCR_W;
        if (x0 == 0 && x1 == SCR_W) memcpy(zbuf + y0 * SCR_W, zstatic + y0 * SCR_W, (y1 - y0) * SCR_W * 2);
        else for (int y = y0; y < y1; y++) memcpy(zbuf + y * SCR_W + x0, zstatic + y * SCR_W + x0, (x1 - x0) * 2);
    }
    r_dx0 = SCR_W; r_dy0 = SCR_H; r_dx1 = 0; r_dy1 = 0;
    r_tris_drawn = 0; r_px = r_spans = r_tests = 0;
}

/* call when the frame is finished: remember what this frame touched */
void r_end(void) {
    int k;
    for (k = 0; k < NBUF; k++) if (buf_ptr[k] == r_fb) break;
    if (r_dx1 <= r_dx0) { r_dx0 = r_dy0 = r_dx1 = r_dy1 = 0; }
    if (k < NBUF) { buf_rect[k][0] = r_dx0; buf_rect[k][1] = r_dy0; buf_rect[k][2] = r_dx1; buf_rect[k][3] = r_dy1; }
    z_rect[0] = r_dx0; z_rect[1] = r_dy0; z_rect[2] = r_dx1; z_rect[3] = r_dy1;
}

/* Draw into the background itself, for what rarely changes (the base, settled
   blocks, most of the HUD). r_bake_begin() starts again from the clean gradient
   with empty depth; until r_bake_end(), r_mesh and the d_* calls draw there. */
static uint16_t* bake_saved;
static int bake_dirty[4];
void r_bake_begin(void) {
    memcpy(sky, sky_clean, sizeof sky);
    memset(zstatic, 0, sizeof zstatic);
    bake_saved = r_fb; r_fb = sky; zbuf = zstatic;
    bake_dirty[0] = r_dx0; bake_dirty[1] = r_dy0; bake_dirty[2] = r_dx1; bake_dirty[3] = r_dy1;
}
void r_bake_end(void) {
    r_fb = bake_saved; zbuf = zbuf_mem;
    r_dx0 = bake_dirty[0]; r_dy0 = bake_dirty[1]; r_dx1 = bake_dirty[2]; r_dy1 = bake_dirty[3];
    r_invalidate();
    z_rect[0] = 0; z_rect[1] = 0; z_rect[2] = SCR_W; z_rect[3] = SCR_H;
}

void r_camera(vec3 eye, vec3 target, float fov) {
    vec3 f = vnorm(vsub(target, eye));            /* forward */
    vec3 r = vnorm(vcross(f, v3(0, 1, 0)));       /* right */
    vec3 u = vcross(r, f);                        /* up */
    V[0] = r.x; V[1] = r.y; V[2] = r.z; V[3] = -vdot(r, eye);
    V[4] = u.x; V[5] = u.y; V[6] = u.z; V[7] = -vdot(u, eye);
    V[8] = f.x; V[9] = f.y; V[10] = f.z; V[11] = -vdot(f, eye);
    focal = (SCR_H * 0.5f) / tanf(fov * 0.5f);
    cam_eye = eye;
    Hh = vnorm(v3(L.x - f.x, L.y - f.y, L.z - f.z));
}

static inline void to_view(vec3 p, float* o) {
    o[0] = V[0] * p.x + V[1] * p.y + V[2] * p.z + V[3];
    o[1] = V[4] * p.x + V[5] * p.y + V[6] * p.z + V[7];
    o[2] = V[8] * p.x + V[9] * p.y + V[10] * p.z + V[11];
}

bool r_project(vec3 p, float* sx, float* sy, float* depth) {
    float v[3]; to_view(p, v);
    if (v[2] < NEAR) return false;
    float iz = 1.0f / v[2];
    *sx = SCR_W * 0.5f + v[0] * focal * iz;
    *sy = SCR_H * 0.5f - v[1] * focal * iz;
    if (depth) *depth = v[2];
    return true;
}

vec3 r_unproject_z(float sx, float sy, float pz) {
    /* ray through the pixel in view space, back to world */
    float dx = (sx - SCR_W * 0.5f) / focal, dy = -(sy - SCR_H * 0.5f) / focal;
    /* world dir = r*dx + u*dy + f */
    vec3 d = v3(V[0] * dx + V[4] * dy + V[8], V[1] * dx + V[5] * dy + V[9], V[2] * dx + V[6] * dy + V[10]);
    if (fabsf(d.z) < 1e-6f) return cam_eye;
    float t = (pz - cam_eye.z) / d.z;
    return v3(cam_eye.x + d.x * t, cam_eye.y + d.y * t, pz);
}

/* ---------------------------------------------------------------- raster */
typedef struct { float x, y, iz; } SV;     /* screen vertex: x, y, 1/z */

static int rmode;


/* float -> int rounding for the pixel centres, without libm (x >= 0) */
static inline int iround(float x) { return (int)(x + 0.5f); }

/* flat-colour triangle with depth test (depth = 1/z, larger is nearer) */
static void raster_tri(const SV* a, const SV* b, const SV* c, uint16_t col) {
    /* sort by y */
    if (a->y > b->y) { const SV* t = a; a = b; b = t; }
    if (b->y > c->y) { const SV* t = b; b = c; c = t; }
    if (a->y > b->y) { const SV* t = a; a = b; b = t; }
    if (c->y - a->y < 0.01f) return;
    if (c->y < 0 || a->y > SCR_H) return;
    float minx = a->x < b->x ? a->x : b->x; if (c->x < minx) minx = c->x;
    float maxx = a->x > b->x ? a->x : b->x; if (c->x > maxx) maxx = c->x;
    if (maxx < 0 || minx > SCR_W) return;
    /* plane equation for 1/z: iz = A*x + B*y + C */
    float x1 = b->x - a->x, y1 = b->y - a->y, z1 = b->iz - a->iz;
    float x2 = c->x - a->x, y2 = c->y - a->y, z2 = c->iz - a->iz;
    float den = x1 * y2 - x2 * y1;
    if (den > -1e-6f && den < 1e-6f) return;
    float rden = 1.0f / den;
    float A = (z1 * y2 - z2 * y1) * rden, B = (x1 * z2 - x2 * z1) * rden;
    const float ZS = 65535.0f * NEAR * 0.98f * 65536.0f;   /* iz*ZS fits 16.16 for z >= NEAR */
    float ay = a->y < 0 ? 0 : a->y;
    int y0 = iround(ay), yend = iround(c->y < SCR_H ? c->y : SCR_H);
    if (y0 >= yend) return;
    float ilong = (c->x - a->x) / (c->y - a->y);
    float ishort1 = (b->y - a->y) > 1e-6f ? (b->x - a->x) / (b->y - a->y) : 0;
    float ishort2 = (c->y - b->y) > 1e-6f ? (c->x - b->x) / (c->y - b->y) : 0;
    bool long_left = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y) > 0;   /* b to the right of a->c */
    int32_t dz = (int32_t)(A * ZS);
    float AZ = A * ZS, BZ = B * ZS;
    float z_origin = a->iz * ZS - AZ * a->x - BZ * a->y;   /* z(x, y) = z_origin + AZ x + BZ y */
    int ghost = rmode & RM_GHOST, nodepth = rmode & RM_NODEPTH;
    uint32_t col2 = col;
    dirty(minx < 0 ? 0 : (int)minx, y0, maxx >= SCR_W ? SCR_W : (int)maxx + 1, yend);
    float py = y0 + 0.5f;
    float xl = a->x + (py - a->y) * ilong;
    float xs = py < b->y ? a->x + (py - a->y) * ishort1 : b->x + (py - b->y) * ishort2;
    int y = y0;
    int ymid = iround(b->y < 0 ? 0 : b->y);
    if (ymid > yend) ymid = yend;
    for (int pass = 0; pass < 2; pass++) {
        int ylim = pass == 0 ? ymid : yend;
        float is = pass == 0 ? ishort1 : ishort2;
        if (pass == 1 && y < ylim) xs = b->x + (y + 0.5f - b->y) * ishort2;
        for (; y < ylim; y++, xl += ilong, xs += is) {
            float xa = long_left ? xl : xs, xb = long_left ? xs : xl;
            if (xa < 0) xa = 0;
            if (xb > SCR_W) xb = SCR_W;
            int x0 = iround(xa), x1i = iround(xb);
            if (x0 >= x1i) continue;
            uint16_t* zp = zbuf + y * SCR_W + x0;
            uint16_t* fp = r_fb + y * SCR_W + x0;
            int n = x1i - x0;
            r_px += n; r_spans++;
            if (nodepth) {
                for (int i = 0; i < n; i++) { fp[i] = col2; zp[i] = 0xFFFF; }
                continue;
            }
            float zf = z_origin + AZ * (x0 + 0.5f) + BZ * (y + 0.5f);
            uint32_t z = zf > 0 ? (uint32_t)zf : 0;
            if (ghost) {
                for (int i = 0; i < n; i++, z += dz) {
                    if ((uint16_t)(z >> 16) > zp[i]) {
                        uint16_t d = fp[i];
                        fp[i] = (uint16_t)((((d & 0xF7BC) >> 1) + ((col2 & 0xF7BC) >> 1)) | 1);
                    }
                }
            } else {
                uint16_t* end = fp + n;
                do {
                    uint32_t zz = z >> 16;
                    if (zz > *zp) { *zp = (uint16_t)zz; *fp = (uint16_t)col2; }
                    z += dz; zp++; fp++;
                } while (fp < end);
            }
        }
    }
    r_tris_drawn++;
}

/* clip a view-space polygon against the near plane and rasterise it */
static void draw_view_tri(const float* p0, const float* p1, const float* p2, uint16_t col) {
    const float* P[3] = { p0, p1, p2 };
    float out[4][3]; int n = 0;
    for (int i = 0; i < 3; i++) {
        const float* a = P[i]; const float* b = P[(i + 1) % 3];
        bool ain = a[2] >= NEAR, bin = b[2] >= NEAR;
        if (ain) { memcpy(out[n++], a, 12); }
        if (ain != bin) {
            float t = (NEAR - a[2]) / (b[2] - a[2]);
            out[n][0] = a[0] + (b[0] - a[0]) * t; out[n][1] = a[1] + (b[1] - a[1]) * t; out[n][2] = NEAR; n++;
        }
    }
    if (n < 3) return;
    SV s[4];
    for (int i = 0; i < n; i++) {
        float iz = 1.0f / out[i][2];
        s[i].x = SCR_W * 0.5f + out[i][0] * focal * iz;
        s[i].y = SCR_H * 0.5f - out[i][1] * focal * iz;
        s[i].iz = iz;
    }
    raster_tri(&s[0], &s[1], &s[2], col);
    if (n == 4) raster_tri(&s[0], &s[2], &s[3], col);
}

static uint16_t shade(uint32_t base, vec3 n, int mode) {
    float d = vdot(n, L);
    float lam = d > 0 ? d : 0;
    float amb = 0.38f + 0.12f * n.y;          /* sky light from above */
    float k = amb + 0.72f * lam;
    float sp = vdot(n, Hh);
    float spec = 0;
    if (sp > 0.82f) { spec = (sp - 0.82f) * 3.2f; spec *= spec; }
    float r = ((base >> 16) & 255) * k + 255 * spec;
    float g = ((base >> 8) & 255) * k + 255 * spec;
    float b = (base & 255) * k + 255 * spec;
    if (mode & RM_FLASH) { r = r * 0.6f + 110; g = g * 0.6f + 110; b = b * 0.6f + 110; }
    int ri = r > 255 ? 255 : (int)r, gi = g > 255 ? 255 : (int)g, bi = b > 255 ? 255 : (int)b;
    return rgb16((ri << 16) | (gi << 8) | bi);
}

#define MAXV 256
void r_mesh(const Mesh* m, const float R[9], float sc, vec3 pos, const uint32_t* mc, int mode) {
    static float vv[MAXV][3];
    int nv = m->nv > MAXV ? MAXV : m->nv;
    /* model -> world -> view in one go: VR = V(3x3) * R */
    float M[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            M[i * 4 + j] = (V[i * 4] * R[j] + V[i * 4 + 1] * R[3 + j] + V[i * 4 + 2] * R[6 + j]) * sc;
        M[i * 4 + 3] = V[i * 4] * pos.x + V[i * 4 + 1] * pos.y + V[i * 4 + 2] * pos.z + V[i * 4 + 3];
    }
    for (int i = 0; i < nv; i++) {
        const float* p = m->v[i];
        vv[i][0] = M[0] * p[0] + M[1] * p[1] + M[2] * p[2] + M[3];
        vv[i][1] = M[4] * p[0] + M[5] * p[1] + M[6] * p[2] + M[7];
        vv[i][2] = M[8] * p[0] + M[9] * p[1] + M[10] * p[2] + M[11];
    }
    rmode = mode;
    for (int t = 0; t < m->nt; t++) {
        const uint16_t* T = m->t[t];
        if (T[0] >= nv || T[1] >= nv || T[2] >= nv) continue;
        const float *a = vv[T[0]], *b = vv[T[1]], *c = vv[T[2]];
        r_tests++;
        /* backface: view-space normal vs the vector to the eye */
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        float wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        if (nx * a[0] + ny * a[1] + nz * a[2] <= 0) continue;   /* view space is left-handed */
        const float* n = m->n[t];
        vec3 wn = v3(R[0] * n[0] + R[1] * n[1] + R[2] * n[2], R[3] * n[0] + R[4] * n[1] + R[5] * n[2], R[6] * n[0] + R[7] * n[1] + R[8] * n[2]);
        draw_view_tri(a, b, c, shade(mc[m->mat[t]], wn, mode));
    }
}

/* floor: a grid of quads fading from near_col to far_col with distance */
void r_floor(float y, float z0, float z1, float x0, float x1, uint32_t nc, uint32_t fc) {
    const int NZ = 8, NX = 4;
    rmode = RM_OPAQUE;
    for (int iz = 0; iz < NZ; iz++) {
        float za = z0 + (z1 - z0) * iz / NZ, zb = z0 + (z1 - z0) * (iz + 1) / NZ;
        float t = (iz + 0.5f) / NZ;
        float r = ((nc >> 16) & 255) * (1 - t) + ((fc >> 16) & 255) * t;
        float g = ((nc >> 8) & 255) * (1 - t) + ((fc >> 8) & 255) * t;
        float b = (nc & 255) * (1 - t) + (fc & 255) * t;
        uint16_t col = rgb16(((int)r << 16) | ((int)g << 8) | (int)b);
        for (int ix = 0; ix < NX; ix++) {
            float xa = x0 + (x1 - x0) * ix / NX, xb = x0 + (x1 - x0) * (ix + 1) / NX;
            float p[4][3];
            to_view(v3(xa, y, za), p[0]); to_view(v3(xb, y, za), p[1]);
            to_view(v3(xb, y, zb), p[2]); to_view(v3(xa, y, zb), p[3]);
            draw_view_tri(p[0], p[1], p[2], col);
            draw_view_tri(p[0], p[2], p[3], col);
        }
    }
}

/* blob shadow: darken floor pixels in an ellipse (no depth test, drawn right after the floor) */
void r_shadow(vec3 c, float rx, float rz, int strength) {
    float cx, cy, d;
    if (!r_project(c, &cx, &cy, &d)) return;
    float sx = rx * focal / d, sy;
    float ex, ey, d2;
    if (!r_project(v3(c.x, c.y, c.z + rz), &ex, &ey, &d2)) return;
    sy = fabsf(ey - cy); if (sy < 1) sy = 1;
    int x0 = (int)(cx - sx), x1 = (int)(cx + sx) + 1, y0 = (int)(cy - sy), y1 = (int)(cy + sy) + 1;
    if (x0 < 0) x0 = 0; if (x1 > SCR_W) x1 = SCR_W; if (y0 < 0) y0 = 0; if (y1 > SCR_H) y1 = SCR_H;
    if (x0 < x1 && y0 < y1) dirty(x0, y0, x1, y1);
    for (int y = y0; y < y1; y++) {
        float dy = (y + 0.5f - cy) / sy;
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f - cx) / sx;
            float q = dx * dx + dy * dy;
            if (q >= 1) continue;
            int k = (int)((1 - q) * strength);      /* 0..strength (of 256) */
            uint16_t p = r_fb[y * SCR_W + x];
            int r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
            r = r * (256 - k) >> 8; g = g * (256 - k) >> 8; b = b * (256 - k) >> 8;
            r_fb[y * SCR_W + x] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
        }
    }
}

/* ---------------------------------------------------------------- 2D */
static inline void blend_px(uint16_t* p, int r8, int g8, int b8, int a) {
    if (a <= 0) return;
    if (a >= 256) { *p = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 3) << 6) | ((b8 >> 3) << 1) | 1); return; }
    uint16_t d = *p;
    int dr = ((d >> 11) & 31) << 3, dg = ((d >> 6) & 31) << 3, db = ((d >> 1) & 31) << 3;
    dr += ((r8 - dr) * a) >> 8; dg += ((g8 - dg) * a) >> 8; db += ((b8 - db) * a) >> 8;
    *p = (uint16_t)(((dr >> 3) << 11) | ((dg >> 3) << 6) | ((db >> 3) << 1) | 1);
}

void d_rect(int x, int y, int w, int h, uint32_t col, int alpha) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > SCR_W) x1 = SCR_W; if (y1 > SCR_H) y1 = SCR_H;
    int r = (col >> 16) & 255, g = (col >> 8) & 255, b = col & 255;
    dirty(x, y, x1, y1);
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++) blend_px(&r_fb[yy * SCR_W + xx], r, g, b, alpha);
}

void d_round_rect(int x, int y, int w, int h, int rad, uint32_t col, int alpha) {
    int r = (col >> 16) & 255, g = (col >> 8) & 255, b = col & 255;
    dirty(x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > SCR_W ? SCR_W : x + w, y + h > SCR_H ? SCR_H : y + h);
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy; if (py < 0 || py >= SCR_H) continue;
        int inset = 0;
        int dy = yy < rad ? rad - yy : (yy >= h - rad ? yy - (h - rad - 1) : 0);
        if (dy) { float q = (float)(rad * rad - dy * dy); inset = rad - (q > 0 ? (int)(sqrtf(q) + 0.5f) : 0); }
        for (int xx = inset; xx < w - inset; xx++) {
            int px = x + xx; if (px < 0 || px >= SCR_W) continue;
            blend_px(&r_fb[py * SCR_W + px], r, g, b, alpha);
        }
    }
}

void d_circle(int cx, int cy, int rad, uint32_t col, int alpha) {
    int r = (col >> 16) & 255, g = (col >> 8) & 255, b = col & 255;
    dirty(cx - rad < 0 ? 0 : cx - rad, cy - rad < 0 ? 0 : cy - rad, cx + rad + 1 > SCR_W ? SCR_W : cx + rad + 1, cy + rad + 1 > SCR_H ? SCR_H : cy + rad + 1);
    for (int dy = -rad; dy <= rad; dy++) {
        int py = cy + dy; if (py < 0 || py >= SCR_H) continue;
        float q = rad * rad - dy * dy + 0.5f * rad;
        int hw = q > 0 ? (int)sqrtf(q) : 0;
        for (int dx = -hw; dx <= hw; dx++) {
            int px = cx + dx; if (px < 0 || px >= SCR_W) continue;
            blend_px(&r_fb[py * SCR_W + px], r, g, b, alpha);
        }
    }
}

static const Glyph* glyph(const Font* f, char ch) {
    int c = (unsigned char)ch - f->first;
    if (c < 0 || c >= f->count) return 0;
    return &f->g[c];
}

int d_text_width(const Font* f, const char* s) {
    int w = 0;
    for (; *s; s++) { const Glyph* g = glyph(f, *s); if (g) w += g->adv; }
    return w;
}

int d_text(const Font* f, int x, int y, const char* s, uint32_t col, int alpha) {
    int r = (col >> 16) & 255, g8 = (col >> 8) & 255, b = col & 255;
    uint16_t solid = rgb16(col);
    int x0 = x;
    for (; *s; s++) {
        const Glyph* g = glyph(f, *s);
        if (!g) continue;
        const uint8_t* d = f->data + g->off;
        int k = 0;
        for (int yy = 0; yy < g->h; yy++) {
            int py = y + g->yo + yy;
            for (int xx = 0; xx < g->w; xx++, k++) {
                int a4 = (k & 1) ? (d[k >> 1] & 15) : (d[k >> 1] >> 4);
                if (!a4) continue;
                int px = x + g->xo + xx;
                if (px < 0 || px >= SCR_W || py < 0 || py >= SCR_H) continue;
                if (a4 == 15 && alpha >= 256) r_fb[py * SCR_W + px] = solid;
                else blend_px(&r_fb[py * SCR_W + px], r, g8, b, (a4 * 17 * alpha) >> 8);
            }
        }
        x += g->adv;
    }
    if (x > x0) dirty(x0 < 0 ? 0 : x0, y < 0 ? 0 : y, x > SCR_W ? SCR_W : x + 2, y + f->line > SCR_H ? SCR_H : y + f->line);
    return x - x0;
}

void d_text_c(const Font* f, int cx, int y, const char* s, uint32_t col, int alpha) {
    d_text(f, cx - d_text_width(f, s) / 2, y, s, col, alpha);
}

void d_logo(int x, int y, int alpha) {
    dirty(x < 0 ? 0 : x, y < 0 ? 0 : y, x + LOGO_W > SCR_W ? SCR_W : x + LOGO_W, y + LOGO_H > SCR_H ? SCR_H : y + LOGO_H);
    for (int yy = 0; yy < LOGO_H; yy++) {
        int py = y + yy; if (py < 0 || py >= SCR_H) continue;
        for (int xx = 0; xx < LOGO_W; xx++) {
            int px = x + xx; if (px < 0 || px >= SCR_W) continue;
            int a = logo_alpha[yy * LOGO_W + xx];
            if (!a) continue;
            uint16_t c = logo_rgba[yy * LOGO_W + xx];
            blend_px(&r_fb[py * SCR_W + px], ((c >> 11) & 31) << 3, ((c >> 6) & 31) << 3, ((c >> 1) & 31) << 3, (a * alpha) >> 8);
        }
    }
}

void d_button(int cx, int cy, int rad, uint32_t col, const char* label) {
    d_circle(cx, cy + 1, rad, 0x000000, 110);
    d_circle(cx, cy, rad, col, 256);
    d_circle(cx - rad / 3, cy - rad / 3, rad / 3, 0xFFFFFF, 60);
    d_text_c(&font_small, cx, cy - font_small.ascent / 2 - 2, label, 0xFFFFFF, 256);
}

/* ---------------------------------------------------------------- sprites */
/* Render a mesh once, seen from the front, into a sprite with a dark outline.
   Pixel value 0 is transparent (every drawn colour has the 5551 alpha bit set). */
void r_make_sprite(const Mesh* m, const float R[9], float scale, const uint32_t* cols, Sprite* out) {
    uint16_t* save_fb = r_fb;
    float saveV[12]; memcpy(saveV, V, sizeof V); float save_focal = focal;
    memset(zbuf_mem, 0, sizeof zbuf_mem);
    memset(zstatic, 0, sizeof zstatic);
    r_fb = zbuf_mem; zbuf = zstatic;
    r_camera(v3(0, 0, 10), v3(0, 0, 0), 2 * atanf(120.0f / 200.0f));    /* 20 px per unit */
    r_mesh(m, R, scale, v3(0, 0, 0), cols, RM_OPAQUE);
    int x0 = SCR_W, y0 = SCR_H, x1 = -1, y1 = -1;
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++)
            if (zbuf_mem[y * SCR_W + x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    x0--; y0--; x1++; y1++;            /* room for the outline */
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w > SPRITE_MAX) w = SPRITE_MAX; if (h > SPRITE_MAX) h = SPRITE_MAX;
    out->w = w; out->h = h; out->hx = SCR_W / 2 - x0; out->hy = SCR_H / 2 - y0;
    const uint16_t ink = rgb16(0x141830);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = x0 + x, sy = y0 + y;
            uint16_t p = (sx >= 0 && sy >= 0 && sx < SCR_W && sy < SCR_H) ? zbuf_mem[sy * SCR_W + sx] : 0;
            if (!p) {
                int near = 0;
                for (int dy = -1; dy <= 1 && !near; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int ax = sx + dx, ay = sy + dy;
                        if (ax >= 0 && ay >= 0 && ax < SCR_W && ay < SCR_H && zbuf_mem[ay * SCR_W + ax]) { near = 1; break; }
                    }
                if (near) p = ink;
            }
            out->px[y * SPRITE_MAX + x] = p;
        }
    r_fb = save_fb; zbuf = zbuf_mem;
    memset(zbuf_mem, 0, sizeof zbuf_mem); memset(zstatic, 0, sizeof zstatic);
    memcpy(V, saveV, sizeof V); focal = save_focal;
    z_rect[0] = 0; z_rect[1] = 0; z_rect[2] = SCR_W; z_rect[3] = SCR_H;
    r_invalidate();
}

/* draw a sprite with its hot spot at (cx, cy) */
void d_sprite(const Sprite* s, int cx, int cy) {
    int x0 = cx - s->hx, y0 = cy - s->hy;
    int ya = y0 < 0 ? 0 : y0, yb = y0 + s->h > SCR_H ? SCR_H : y0 + s->h;
    int xa = x0 < 0 ? 0 : x0, xb = x0 + s->w > SCR_W ? SCR_W : x0 + s->w;
    if (xa >= xb || ya >= yb) return;
    dirty(xa, ya, xb, yb);
    for (int y = ya; y < yb; y++) {
        const uint16_t* src = s->px + (y - y0) * SPRITE_MAX + (xa - x0);
        uint16_t* dst = r_fb + y * SCR_W + xa;
        for (int x = xa; x < xb; x++, src++, dst++) if (*src) *dst = *src;
    }
}
