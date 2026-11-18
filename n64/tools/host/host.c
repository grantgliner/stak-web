/* Desktop harness: runs the game with scripted controller input and writes
 * frames as PPM. Script lines:  <frames> <stick x> <stick y> [buttons]   or
 * shot <file.ppm>.  Buttons: A B Z S(tart) L R CU CD CL CR DU DD DL DR, joined by '+'. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "render.h"

static uint16_t fb[SCR_W * SCR_H];
void plat_save(const SaveData* s) { (void)s; }
void plat_load(SaveData* s) { memset(s, 0, sizeof *s); }
void snd_init(void) {}
void snd_update(void) {}
void snd_sfx(int id) { (void)id; }

static uint32_t parse_btn(const char* s) {
    static const char* names[] = { "A", "B", "Z", "S", "L", "R", "CU", "CD", "CL", "CR", "DU", "DD", "DL", "DR" };
    uint32_t m = 0; char buf[128]; strncpy(buf, s, 127); buf[127] = 0;
    for (char* t = strtok(buf, "+"); t; t = strtok(0, "+"))
        for (int i = 0; i < 14; i++) if (!strcmp(t, names[i])) m |= 1u << i;
    return m;
}

static void shot(const char* fn) {
    FILE* f = fopen(fn, "wb"); fprintf(f, "P6 %d %d 255\n", SCR_W, SCR_H);
    for (int i = 0; i < SCR_W * SCR_H; i++) { uint16_t p = fb[i]; unsigned char c[3] = { ((p >> 11) & 31) << 3, ((p >> 6) & 31) << 3, ((p >> 1) & 31) << 3 }; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(int argc, char** argv) {
    FILE* sf = fopen(argv[1], "r");
    game_init();
    char line[256]; uint32_t prev = 0; int total = 0;
    while (fgets(line, sizeof line, sf)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char fn[200];
        if (sscanf(line, "shot %199s", fn) == 1) { shot(fn); { extern int r_px, r_spans, r_tests; printf("%s at frame %d (tris %d px %d spans %d tests %d)\n", fn, total, r_tris_drawn, r_px, r_spans, r_tests); } continue; }
        int n; float x, y; char b[128] = "";
        if (sscanf(line, "%d %f %f %127s", &n, &x, &y, b) < 3) continue;
        uint32_t btn = parse_btn(b);
        for (int i = 0; i < n; i++) {
            Input in = { x, y, btn, i == 0 ? btn & ~prev : 0 };
            if (i == 0) prev = btn;
            game_update(&in, 1);
            game_draw(fb);
            total++;
        }
        if (n == 0) prev = btn;
    }
    return 0;
}
