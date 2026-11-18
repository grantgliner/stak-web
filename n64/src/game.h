/* STAK! N64 game logic and drawing, independent of libdragon so the same code
 * runs in the ROM (main.c) and in a desktop test harness (tools/host/). */
#pragma once
#include <stdint.h>

enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_Z = 1 << 2, BTN_START = 1 << 3,
    BTN_L = 1 << 4, BTN_R = 1 << 5,
    BTN_CU = 1 << 6, BTN_CD = 1 << 7, BTN_CL = 1 << 8, BTN_CR = 1 << 9,
    BTN_DU = 1 << 10, BTN_DD = 1 << 11, BTN_DL = 1 << 12, BTN_DR = 1 << 13,
};

typedef struct {
    float sx, sy;          /* analog stick, -1..1 after the dead zone (+y is up) */
    uint32_t held, down;   /* buttons held now, and newly pressed this frame */
} Input;

typedef struct {
    uint32_t magic;
    uint16_t best_blocks;
    uint16_t pad;
    uint32_t best_score;
} SaveData;

void game_init(void);
/* one frame: input, then `steps` physics steps of 1/60 s (0 while paused) */
void game_update(const Input* in, int steps);
void game_draw(uint16_t* fb);

/* provided by the platform */
void plat_save(const SaveData* s);
void plat_load(SaveData* s);
