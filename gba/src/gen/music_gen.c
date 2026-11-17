#include "music_gen.h"
static const u8 lead[] = { 72, 8, 76, 8, 79, 8, 76, 8 };
static const u8 bass[] = { 48, 16, 43, 16 };
static const u8 drums[] = { 1, 8, 3, 8, 2, 8, 3, 8 };
const Song song_menu = { lead, sizeof lead, 0, 0, bass, sizeof bass, drums, sizeof drums, 4, 2, 0xA2, 1 };
const Song song_classic = { lead, sizeof lead, 0, 0, bass, sizeof bass, drums, sizeof drums, 3, 2, 0xA2, 1 };
