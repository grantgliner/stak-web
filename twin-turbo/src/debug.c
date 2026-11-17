#include "gba.h"
#include <stdarg.h>
#include <stdio.h>
/* mGBA debug output: https://github.com/mgba-emu/mgba/blob/master/opt/libgba/mgba.c */
#define REG_DEBUG_ENABLE (*(vu16*)0x4FFF780)
#define REG_DEBUG_FLAGS  (*(vu16*)0x4FFF700)
#define REG_DEBUG_STRING ((char*)0x4FFF600)
void dbg_printf(const char* fmt, ...) {
  REG_DEBUG_ENABLE = 0xC0DE;
  if (REG_DEBUG_ENABLE != 0x1DEA) return;
  va_list a; va_start(a, fmt);
  vsnprintf(REG_DEBUG_STRING, 0x100, fmt, a);
  va_end(a);
  REG_DEBUG_FLAGS = 2 | 0x100;   /* level: info */
}
