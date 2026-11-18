#!/bin/sh
# build the desktop harness: tools/host/build.sh && tools/host/stakhost script.txt
cd "$(dirname "$0")/../.."
gcc -O2 -g -std=gnu11 -DN64_PORT -DHOST -include stdio.h -Isrc -I../gba/src -w -o tools/host/stakhost \
  tools/host/host.c src/game.c src/render.c src/gen/assets_gen.c \
  ../gba/src/physics.iwram.c ../gba/src/gen/pieces_gen.c ../gba/src/gen/tables_gen.c -lm
