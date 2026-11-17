/* Sequential stacking test: place a block on top every N steps like a player would. */
#include <stdio.h>
#include <stdlib.h>
#include "../../src/physics.h"
#include "../../src/gen/pieces_gen.h"
static double f(fx v) { return v / 65536.0; }
int main(int argc, char** argv) {
  int n = argc > 1 ? atoi(argv[1]) : 12;
  double off = argc > 2 ? atof(argv[2]) : 0.3;
  int shape = argc > 3 ? atoi(argv[3]) : 1;
  int wait = argc > 4 ? atoi(argv[4]) : 90;
  phys_init();
  phys_add_static_box(0, -FX_ONE, FX(4.1), FX_ONE);
  int maxpts = 0, fell = 0; long work = 0;
  for (int k = 0; k < n; k++) {
    fx top = 0;
    for (int i = 1; i < phys_count; i++) if (phys_bodies[i].bmax.y > top) top = phys_bodies[i].bmax.y;
    int sh = shape < 0 ? (k * 5) % PIECE_COUNT : shape;
    phys_add_piece(sh, 0, (k & 1) ? FX(off) : FX(-off), top + piece_defs[sh].bottom + FX(0.75), 0);
    for (int s = 0; s < wait; s++) { phys_step(); if (phys_points > maxpts) maxpts = phys_points; work += phys_points; }
    for (int i = 1; i < phys_count; i++) if (phys_bodies[i].p.y < FX(-4)) fell = 1;
    if (fell) { printf("FELL after %d blocks\n", k + 1); break; }
  }
  fx top = 0; for (int i = 1; i < phys_count; i++) if (phys_bodies[i].bmax.y > top) top = phys_bodies[i].bmax.y;
  printf("n=%d top=%.2f maxpts=%d avgpts=%.1f awake=%d\n", n, f(top), maxpts, work / (double)(n * wait), phys_awake);
}
