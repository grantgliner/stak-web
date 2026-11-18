/* STAK! N64: libdragon glue (display, controller, EEPROM, timing). */
#include <libdragon.h>
#include <string.h>
#include "game.h"
#include "render.h"
#include "audio.h"

static bool have_eeprom;

#ifdef DEMO
/* scripted input for testing in emulators without a controller: frames, stick, buttons */
typedef struct { int n; float x, y; uint32_t b; } Step;
static const Step demo[] = {
    { 90, 0, 0, 0 }, { 1, 0, 0, BTN_START }, { 40, 0, 0, 0 },
    { 1, 0, 0, BTN_DL }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 30, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 90, 0, 0, 0 },
    { 1, 0, 0, BTN_DR }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 30, 0, 0.5f, 0 }, { 1, 0, 0, BTN_A }, { 90, 0, 0, 0 },
    { 1, 0, 0, BTN_DR }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 30, 0, 0.5f, 0 }, { 1, 0, 0, BTN_R }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 90, 0, 0, 0 },
    { 1, 0, 0, BTN_DL }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 30, 0, 0.6f, 0 }, { 1, 0, 0, BTN_A }, { 90, 0, 0, 0 },
    { 1, 0, 0, BTN_DR }, { 20, 0, 0, 0 }, { 1, 0, 0, BTN_A }, { 26, 0, 0, 0 }, { 30, 1, 0, 0 }, { 1, 0, 0, BTN_A }, { 600, 0, 0, 0 },
};
static void demo_input(Input* in) {
    static int i, k;
    if (i >= (int)(sizeof demo / sizeof demo[0])) { *in = (Input){ 0 }; return; }
    in->sx = demo[i].x; in->sy = demo[i].y; in->held = demo[i].b; in->down = k == 0 ? demo[i].b : 0;
    if (++k >= demo[i].n) { k = 0; i++; }
}
#endif

void plat_load(SaveData* s) {
    memset(s, 0, sizeof *s);
    if (have_eeprom) eeprom_read_bytes(s, 0, sizeof *s);
}

void plat_save(const SaveData* s) {
    if (have_eeprom) eeprom_write_bytes(s, 0, sizeof *s);
}

static float stick(int v) {
    /* dead zone, and full deflection at about 70 (real sticks rarely reach 85) */
    float f = v / 70.0f;
    if (f > 1) f = 1; if (f < -1) f = -1;
    if (f > -0.12f && f < 0.12f) return 0;
    return f > 0 ? (f - 0.12f) / 0.88f : (f + 0.12f) / 0.88f;
}

static uint32_t btn_mask(joypad_buttons_t b) {
    uint32_t m = 0;
    if (b.a) m |= BTN_A; if (b.b) m |= BTN_B; if (b.z) m |= BTN_Z; if (b.start) m |= BTN_START;
    if (b.l) m |= BTN_L; if (b.r) m |= BTN_R;
    if (b.c_up) m |= BTN_CU; if (b.c_down) m |= BTN_CD; if (b.c_left) m |= BTN_CL; if (b.c_right) m |= BTN_CR;
    if (b.d_up) m |= BTN_DU; if (b.d_down) m |= BTN_DD; if (b.d_left) m |= BTN_DL; if (b.d_right) m |= BTN_DR;
    return m;
}

#ifdef PROFILE
/* -DPROFILE: average time per frame section on the debug output, every 2 s */
extern void (*game_prof)(int);
static uint64_t prof_t[8], prof_sum[8];
static void prof_cb(int k) { prof_t[k] = get_ticks_us(); if (k) prof_sum[k] += prof_t[k] - prof_t[k - 1]; }
#endif

int main(void) {
    debug_init_isviewer();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    have_eeprom = eeprom_present() != EEPROM_NONE;
    snd_init();
    game_init();
#ifdef PROFILE
    game_prof = prof_cb;
    uint32_t frames = 0, work_us = 0;
#endif

    /* the game ticks at 60 Hz whatever the frame rate: a slow frame runs extra ticks (up to 3) */
    uint64_t last = get_ticks_us();
    float acc = 0;
    while (1) {
        joypad_poll();
        joypad_inputs_t ji = joypad_get_inputs(JOYPAD_PORT_1);
        Input in;
        in.sx = stick(ji.stick_x);
        in.sy = stick(ji.stick_y);
        in.held = btn_mask(joypad_get_buttons_held(JOYPAD_PORT_1));
        in.down = btn_mask(joypad_get_buttons_pressed(JOYPAD_PORT_1));
#ifdef DEMO
        demo_input(&in);
#endif
        uint64_t now = get_ticks_us();
        acc += (now - last) / 16666.7f;
        last = now;
        int steps = (int)acc;
        if (steps > 3) { steps = 3; acc = 0; } else acc -= steps;
        if (steps < 1) { steps = 1; acc = 0; }
#ifdef PROFILE
        uint64_t t0 = get_ticks_us();
#endif
        for (int k = 0; k < steps; k++) {
            game_update(&in, 1);
            in.down = 0;
#ifdef DEMO
            if (k + 1 < steps) demo_input(&in), in.down = 0;
#endif
        }
#ifdef PROFILE
        prof_sum[7] += get_ticks_us() - t0;
#endif
        surface_t* d = display_get();
        /* draw through the cached alias (uncached writes cost a bus trip each on
           hardware), then write the lines back before the VI reads them */
        uint16_t* fbp = (uint16_t*)CachedAddr(d->buffer);
        game_draw(fbp);
        data_cache_hit_writeback(fbp, SCR_W * SCR_H * 2);
#ifdef PROFILE
        work_us += (uint32_t)(get_ticks_us() - t0);
#endif
        display_show(d);
        snd_update();
#ifdef PROFILE
        if (++frames % 120 == 0) {
            debugf("frame work %lu us, tris %d | update %lu begin %lu base %lu bodies+tray %lu hand %lu popup %lu overlays %lu\n", (unsigned long)(work_us / 120), r_tris_drawn,
                   (unsigned long)(prof_sum[7] / 120), (unsigned long)(prof_sum[1] / 120), (unsigned long)(prof_sum[2] / 120), (unsigned long)(prof_sum[3] / 120),
                   (unsigned long)(prof_sum[4] / 120), (unsigned long)(prof_sum[5] / 120), (unsigned long)(prof_sum[6] / 120));
            work_us = 0; memset(prof_sum, 0, sizeof prof_sum);
        }
#endif
    }
}
