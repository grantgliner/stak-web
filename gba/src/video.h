#pragma once
#include "gba.h"

/* VRAM layout */
#define CBB_TEXT   0          /* BG0: font, big digits, panel tiles */
#define CBB_STARS  1          /* BG2 */
#define CBB_LOGO   2          /* BG1 (8bpp, spills into CBB 3) */
#define SBB_STARS  29
#define SBB_LOGO   30
#define SBB_TEXT   31

#define TILE_FONT    0        /* 96 tiles, ASCII 32.. */
#define TILE_BIG     96       /* 40 tiles (10 digits x 2x2) */
#define TILE_PANEL   136      /* 9 tiles */
#define TILE_FONT_PANEL 160   /* font with panel-coloured background */
#define TXT_ON_PANEL 0x100

#define OBJ_TILE_PIECE(n) ((n) * 64)
#define OBJ_TILE_BASE  768
#define OBJ_TILE_ARROW 832
#define OBJ_TILE_FONT  840

#define PAL_TXT_WHITE 0
#define PAL_TXT_YELLOW 1
#define PAL_TXT_GRAY 2
#define PAL_PANEL 3
#define PAL_PANEL_HI 4

#define OBJPAL_RED 7
#define OBJPAL_BASE 8
#define OBJPAL_UI 9

extern ObjAttr oam_shadow[128];
extern u16 txt_map[32 * 32];
extern int txt_dirty;

void video_init(void);
void video_vsync(int sky_offset);    /* wait for vblank and push shadows */
extern volatile u16 vblank_tick;      /* timer 2 (64-cycle ticks) when the last VBlank began */
#define FRAME_TICKS 4389              /* 280896 cycles */
void oam_clear(void);
ObjAttr* oam_alloc(void);           /* next free OAM entry (or NULL) */
void oam_reset_alloc(int first);
void obj_affine_set(int idx, s32 cos16, s32 sin16, s32 scale8);  /* scale8: 256 = 1x display size */

void txt_clear(void);
void txt_clear_rect(int x, int y, int w, int h);
void txt_put(int x, int y, const char* s, int pal);
int  txt_wrap(int x, int y, int w, const char* s, int pal);   /* returns lines used */
void txt_num(int x, int y, int value, int pal);
void txt_big_num(int xr, int y, int value, int pal);           /* right-aligned at tile xr */
void txt_panel(int x, int y, int w, int h, int pal);
void txt_center(int y, int x0, int w, const char* s, int pal);
int  fmt_int(char* buf, int v);
