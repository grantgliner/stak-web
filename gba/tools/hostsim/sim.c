/* Host-side physics test: runs the exact GBA physics code natively. */
#include <stdio.h>
#include <stdlib.h>
#include "../../src/physics.h"
#include "../../src/gen/pieces_gen.h"
static double f(fx v) { return v / 65536.0; }
int main(int argc, char** argv) {
  int n = argc > 1 ? atoi(argv[1]) : 8;
  double off = argc > 2 ? atof(argv[2]) : 0.3;
  int shape = argc > 3 ? atoi(argv[3]) : 1;
  phys_init();
  phys_add_static_box(0, -FX_ONE, FX(4.1), FX_ONE);
  for (int k = 0; k < n; k++) phys_add_piece(shape, 0, (k & 1) ? FX(off) : FX(-off), FX(1.52) + k * FX(3.02), 0);
  for (int s = 0; s <= 600; s++) {
    phys_step();
    if (s % (argc > 4 ? atoi(argv[4]) : 60) == 0 && s < (argc > 5 ? atoi(argv[5]) : 99999)) {
      printf("t=%3d awake=%2d pts=%3d ", s, phys_awake, phys_points);
      for (int i = 1; i < phys_count; i += (phys_count > 9 ? 3 : 1)) printf("[%5.2f %5.2f %5.2f] ", f(phys_bodies[i].p.x), f(phys_bodies[i].p.y), f(phys_bodies[i].a));
      printf("\n");
    }
  }
}
