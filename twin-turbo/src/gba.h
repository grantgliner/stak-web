/* Minimal GBA hardware definitions for Twin Turbo (no external libs needed). */
#pragma once
#include <stdint.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef int64_t s64;  typedef uint64_t u64;
typedef volatile u16 vu16; typedef volatile u32 vu32;

#define IWRAM_CODE __attribute__((section(".iwram"), long_call, noinline))
#define EWRAM_BSS  __attribute__((section(".sbss")))
#define INLINE static inline __attribute__((always_inline))

#define MEM_IO    0x04000000
#define MEM_PAL   0x05000000
#define MEM_VRAM  0x06000000
#define MEM_OAM   0x07000000
#define MEM_SRAM  0x0E000000

#define REG_DISPCNT  (*(vu16*)(MEM_IO+0x0000))
#define REG_DISPSTAT (*(vu16*)(MEM_IO+0x0004))
#define REG_VCOUNT   (*(vu16*)(MEM_IO+0x0006))
#define REG_BGCNT    ((vu16*)(MEM_IO+0x0008))
#define REG_BG0HOFS  (*(vu16*)(MEM_IO+0x0010))
#define REG_BG0VOFS  (*(vu16*)(MEM_IO+0x0012))
#define REG_BGHOFS(n) (*(vu16*)(MEM_IO+0x0010+4*(n)))
#define REG_BGVOFS(n) (*(vu16*)(MEM_IO+0x0012+4*(n)))
#define REG_WIN0H    (*(vu16*)(MEM_IO+0x0040))
#define REG_WIN0V    (*(vu16*)(MEM_IO+0x0044))
#define REG_WININ    (*(vu16*)(MEM_IO+0x0048))
#define REG_WINOUT   (*(vu16*)(MEM_IO+0x004A))
#define REG_BG2PA    (*(vu16*)(MEM_IO+0x0020))
#define REG_BG2PB    (*(vu16*)(MEM_IO+0x0022))
#define REG_BG2PC    (*(vu16*)(MEM_IO+0x0024))
#define REG_BG2PD    (*(vu16*)(MEM_IO+0x0026))
#define REG_BG2X     (*(vu32*)(MEM_IO+0x0028))
#define REG_BG2Y     (*(vu32*)(MEM_IO+0x002C))
#define REG_WIN1H    (*(vu16*)(MEM_IO+0x0042))
#define REG_WIN1V    (*(vu16*)(MEM_IO+0x0046))
#define REG_MOSAIC   (*(vu16*)(MEM_IO+0x004C))
#define REG_BLDCNT   (*(vu16*)(MEM_IO+0x0050))
#define REG_BLDALPHA (*(vu16*)(MEM_IO+0x0052))
#define REG_BLDY     (*(vu16*)(MEM_IO+0x0054))

#define REG_SOUND1CNT_L (*(vu16*)(MEM_IO+0x0060))
#define REG_SOUND1CNT_H (*(vu16*)(MEM_IO+0x0062))
#define REG_SOUND1CNT_X (*(vu16*)(MEM_IO+0x0064))
#define REG_SOUND2CNT_L (*(vu16*)(MEM_IO+0x0068))
#define REG_SOUND2CNT_H (*(vu16*)(MEM_IO+0x006C))
#define REG_SOUND3CNT_L (*(vu16*)(MEM_IO+0x0070))
#define REG_SOUND3CNT_H (*(vu16*)(MEM_IO+0x0072))
#define REG_SOUND3CNT_X (*(vu16*)(MEM_IO+0x0074))
#define REG_SOUND4CNT_L (*(vu16*)(MEM_IO+0x0078))
#define REG_SOUND4CNT_H (*(vu16*)(MEM_IO+0x007C))
#define REG_SOUNDCNT_L  (*(vu16*)(MEM_IO+0x0080))
#define REG_SOUNDCNT_H  (*(vu16*)(MEM_IO+0x0082))
#define REG_SOUNDCNT_X  (*(vu16*)(MEM_IO+0x0084))
#define REG_SOUNDBIAS   (*(vu16*)(MEM_IO+0x0088))
#define WAVE_RAM        ((vu32*)(MEM_IO+0x0090))

#define REG_DMA0SAD  (*(vu32*)(MEM_IO+0x00B0))
#define REG_DMA0DAD  (*(vu32*)(MEM_IO+0x00B4))
#define REG_DMA0CNT  (*(vu32*)(MEM_IO+0x00B8))
#define REG_DMA1SAD  (*(vu32*)(MEM_IO+0x00BC))
#define REG_DMA1DAD  (*(vu32*)(MEM_IO+0x00C0))
#define REG_DMA1CNT  (*(vu32*)(MEM_IO+0x00C4))
#define REG_DMA2SAD  (*(vu32*)(MEM_IO+0x00C8))
#define REG_DMA2DAD  (*(vu32*)(MEM_IO+0x00CC))
#define REG_DMA2CNT  (*(vu32*)(MEM_IO+0x00D0))
#define REG_FIFO_A   (*(vu32*)(MEM_IO+0x00A0))
#define DMA_SPECIAL  0x30000000
#define REG_DMA3SAD  (*(vu32*)(MEM_IO+0x00D4))
#define REG_DMA3DAD  (*(vu32*)(MEM_IO+0x00D8))
#define REG_DMA3CNT  (*(vu32*)(MEM_IO+0x00DC))
#define DMA_ENABLE   0x80000000
#define DMA_HBLANK   0x20000000
#define DMA_REPEAT   0x02000000
#define DMA_32       0x04000000
#define DMA_DST_FIXED 0x00400000
#define DMA_DST_RELOAD 0x00600000
#define DMA_SRC_FIXED 0x01000000

#define REG_TM0CNT_L (*(vu16*)(MEM_IO+0x0100))
#define REG_TM0CNT_H (*(vu16*)(MEM_IO+0x0102))
#define REG_TM1CNT_L (*(vu16*)(MEM_IO+0x0104))
#define REG_TM1CNT_H (*(vu16*)(MEM_IO+0x0106))
#define REG_TM2CNT_L (*(vu16*)(MEM_IO+0x0108))
#define REG_TM2CNT_H (*(vu16*)(MEM_IO+0x010A))
#define REG_TM3CNT_L (*(vu16*)(MEM_IO+0x010C))
#define REG_TM3CNT_H (*(vu16*)(MEM_IO+0x010E))

#define REG_KEYINPUT (*(vu16*)(MEM_IO+0x0130))
#define REG_IE       (*(vu16*)(MEM_IO+0x0200))
#define REG_IF       (*(vu16*)(MEM_IO+0x0202))
#define REG_WAITCNT  (*(vu16*)(MEM_IO+0x0204))
#define REG_IME      (*(vu16*)(MEM_IO+0x0208))

#define PAL_BG   ((vu16*)MEM_PAL)
#define PAL_OBJ  ((vu16*)(MEM_PAL+0x200))
#define CHARBLOCK(n) ((vu16*)(MEM_VRAM + 0x4000*(n)))
#define SCREENBLOCK(n) ((vu16*)(MEM_VRAM + 0x800*(n)))
#define OBJ_TILES ((vu16*)(MEM_VRAM + 0x10000))

/* DISPCNT */
#define DCNT_MODE0 0
#define DCNT_MODE1 1
#define DCNT_MODE2 2
#define DCNT_OBJ_1D 0x0040
#define DCNT_BG0 0x0100
#define DCNT_BG1 0x0200
#define DCNT_BG2 0x0400
#define DCNT_BG3 0x0800
#define DCNT_OBJ 0x1000
#define DCNT_WIN0 0x2000
#define DCNT_WIN1 0x4000

/* BGCNT */
#define BG_PRIO(n) (n)
#define BG_CBB(n) ((n)<<2)
#define BG_8BPP 0x0080
#define BG_SBB(n) ((n)<<8)
#define BG_SIZE_32x32 0x0000
#define BG_SIZE_64x32 0x4000
#define BG_SIZE_32x64 0x8000
#define BG_SIZE_64x64 0xC000
#define BG_AFF_128 0x8000   /* affine 1024x1024 */
#define BG_WRAP 0x2000
#define BG_MOSAIC 0x0040

/* keys */
#define KEY_A 0x0001
#define KEY_B 0x0002
#define KEY_SELECT 0x0004
#define KEY_START 0x0008
#define KEY_RIGHT 0x0010
#define KEY_LEFT 0x0020
#define KEY_UP 0x0040
#define KEY_DOWN 0x0080
#define KEY_R 0x0100
#define KEY_L 0x0200

/* OAM */
typedef struct { u16 attr0, attr1, attr2, fill; } ObjAttr;
typedef struct { u16 f0[3]; s16 pa; u16 f1[3]; s16 pb; u16 f2[3]; s16 pc; u16 f3[3]; s16 pd; } ObjAffine;
#define ATTR0_Y(y) ((y) & 0xFF)
#define ATTR0_AFF 0x0100
#define ATTR0_HIDE 0x0200
#define ATTR0_AFF_DBL 0x0300
#define ATTR0_BLEND 0x0400
#define ATTR0_8BPP 0x2000
#define ATTR0_MOSAIC 0x1000
#define ATTR0_SQUARE 0x0000
#define ATTR0_WIDE 0x4000
#define ATTR0_TALL 0x8000
#define ATTR1_X(x) ((x) & 0x1FF)
#define ATTR1_AFFIDX(n) ((n) << 9)
#define ATTR1_HFLIP 0x1000
#define ATTR1_VFLIP 0x2000
#define ATTR1_SIZE_8 0x0000
#define ATTR1_SIZE_16 0x4000
#define ATTR1_SIZE_32 0x8000
#define ATTR1_SIZE_64 0xC000
#define ATTR2_TILE(n) ((n) & 0x3FF)
#define ATTR2_PRIO(n) ((n) << 10)
#define ATTR2_PAL(n) ((n) << 12)

#define RGB15(r,g,b) ((r) | ((g) << 5) | ((b) << 10))

/* BIOS */
INLINE void VBlankIntrWait(void) { __asm__ volatile("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory"); }

/* mGBA debug log (ignored on hardware) */
void dbg_printf(const char* fmt, ...);

/* fast copies */
INLINE void dma3_copy32(volatile void* dst, const void* src, u32 words) {
  REG_DMA3SAD = (u32)src; REG_DMA3DAD = (u32)dst; REG_DMA3CNT = DMA_ENABLE | DMA_32 | words;
}
INLINE void dma3_fill32(volatile void* dst, u32 val, u32 words) {
  static volatile u32 v; v = val;
  REG_DMA3SAD = (u32)&v; REG_DMA3DAD = (u32)dst; REG_DMA3CNT = DMA_ENABLE | DMA_32 | DMA_SRC_FIXED | words;
}
