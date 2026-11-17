# STAK! for Game Boy Advance

A homebrew GBA version of STAK!: stack blocks on the base as high as you can before one falls off. It has the web version's four modes:

- **Relaxed:** simple shapes, at a sunset beach.
- **Classic:** the original pieces, under the stars.
- **Expert:** adds a circle, a semicircle, a lollipop and a seesaw, on black velvet with champagne bubbles.
- **Arcade:** special blocks and changing conditions, under neon confetti.

Each mode has its own music, colours and best score. Unlike the web version, expert is unlocked from the start.

## Controls

| Button | Action |
| --- | --- |
| Left / Right | On the title screen, pick a mode. The game remembers the last one. |
| Up / Down | Choose a block in the tray. The first block is selected automatically. |
| A | Pick up the selected block. It appears just above the tower. |
| D-pad | Move the held block. Hold to speed up. |
| L / R | Rotate the held block 45° left or right. In arcade, a weird block tips toward or away from you instead. |
| A | Drop the block. A red block overlaps something and can't be dropped. |
| B | Put the block back in the tray. |
| Start | Pause. In the pause menu, Select quits to the title screen. |

## Building

You need an `arm-none-eabi` GCC toolchain. devkitARM works, and so does Ubuntu's `gcc-arm-none-eabi` with `libnewlib-arm-none-eabi`.

```sh
make            # builds stak.gba
```

The generated assets in `src/gen/` are committed, so Python is only needed if you change the art or music. To regenerate them, run `make assets`. This needs Python 3 with numpy, Pillow and pretty_midi.

### Real hardware

`tools/gbafix.py` fixes the header checksum, which is enough for emulators. A real GBA or flash cart also checks the Nintendo logo in the header. If devkitPro's `gbafix` is on your `PATH`, the Makefile uses it automatically. Otherwise, run `gbafix stak.gba` yourself before flashing.

## How it works

- **Blocks** are pre-rendered from `models/pieces.glb` by `tools/gen_pieces.py`. Each of the 16 blocks becomes a 4bpp sprite at 12 px per cell, with a bevel and outline. The colours are palettes, so one sprite serves every colour. Rotation uses affine sprites. The same script extracts each block's convex collision parts and mass properties.
  - Most sprites are 64×64 and centred on the block's centre of mass. The circle fits in 32×32. The lollipop is too long for that, so its sprite is centred on its bounding box and the game rotates the offset along with the block.
  - Sprite video memory holds exactly 1024 tiles, so it is packed tightly: 976 tiles of blocks, then the score pop-up digits (`+` to `9` only, 15 tiles), then the left half of the base slab (32 tiles; the right half is the same tiles flipped).
- **Modes and themes:** `tools/gen_gfx.py` builds a theme per mode, and `video_set_theme()` loads it when the mode changes. A theme is:
  - the sky gradient, drawn with HBlank DMA;
  - a scrolling background layer: drifting clouds and gulls for relaxed, the star field for classic, rising champagne bubbles and sparkles for expert;
  - panel and text colours;
  - the block colours, taken from the web version (relaxed: cyan, green, orange, yellow; expert: white, charcoal, gold).
- **Title logo** is an 8bpp background, built by `tools/gen_gfx.py` along with the font and panels.
- **Physics** (`src/physics.iwram.c`) is a fixed-point (Q16.16) 2D rigid-body engine that runs from IWRAM as ARM code.
  - 1 cell is 2.5 cm, and gravity is the real 9.81 m/s².
  - Collision uses SAT polygon tests with clipped two-point manifolds.
  - Round parts (the circle and the lollipop's head) are true circles, with one contact point. A little rolling resistance (like the web version's 48-sided circles) lets them sit still on a slope of up to about 3°. The semicircle is five thin slabs. Round-part collision runs once per frame, so it lives in ROM and leaves IWRAM to the solver.
  - Each frame takes 4 substeps of a sequential-impulse solver, with warm starting and a two-point block solver.
  - Settled blocks sleep and then act as solid ground for blocks placed on top. A static balance check wakes a tower when the combined weight above a block no longer sits over its support, so overhangs still topple. This keeps tall towers steady and keeps the per-frame cost to the few blocks that are moving.
- **Rules** follow the Vision Pro and web versions:
  - each mode's piece pool and colours,
  - a base 0.205 m wide,
  - friction of 0.8 and restitution of 0.25,
  - +250 × n points for the nth block,
  - game over when a block falls below the base or more than 0.45 m to either side,
  - the same game-over messages.
- **Sound** is PSG only. Music uses channels 2, 3 and 4 plus a harmony line on channel 1. Sound effects take over channel 1.
  - All four tunes are original arrangements for the PSG, written in `tools/compose.py`. They borrow the key, tempo, chord colour and motif shapes of `audio/menu.m4a`, `classic.m4a`, `easy.m4a` and `hard.m4a`:
    - menu: C major at about 109 bpm, with the original's borrowed Bb and Eb chords;
    - classic: E dorian at 166 bpm, over Em, C, D and A;
    - relaxed: A major at 94 bpm, with a mixolydian G, half-time drums and off-beat chord stabs;
    - expert: G minor at 110 bpm, with the original's borrowed Gb and Ab chords, a walking bass and a dotted lead.
  - The melodies are written out bar by bar. Harmony (broken chords), bass and drums are generated from each song's chord chart.
  - Running `compose.py` writes `music/<song>.mid`, with lead, harmony, bass and drum tracks. `tools/gen_music.py` (part of `make assets`) turns those into note data, and the whole song loops. Per-song voice settings live in `gen_music.py`: duty, envelope, vibrato and bass wave.
  - `tools/stems_to_midi.py` transcribes the original audio (Demucs stems, then basic-pitch) into `music/transcribed/` for reference. It needs demucs, torch, basic-pitch and librosa.
- **Arcade** follows the web version's rules:
  - **Special blocks:** magnets (+ attracts −, like charges push apart), sticky (holds on to whatever it touches, the base included), explosive (blows up with every block it touches) and weird (tipped with L and R). A tray slot gets a magnet 10% of the time, sticky or explosive 3% each and weird 2%. The opening tray always has one, and a refill after three plain blocks in a row is always special.
  - **Conditions:** after a few calm turns (four at the start, then two to four), a condition is announced three turns ahead and then lasts three turns. It is never the same one twice in a row: giant pieces (1.5×), tiny pieces (0.5×), weird rotation, magnet storm, sticky situation or powder keg (everything explosive). The score panel counts down to it, a banner shows it, and the music changes with it (pitched up or down, wobbly, thin and metallic, muffled, or rumbling).
  - **Piece pool** starts with relaxed's shapes, adds classic's from 10 blocks and expert's from 30.
  - **Physics:** gravity is 0.85 g, friction 0.9 and restitution almost zero. Magnets use the web version's force law between block outlines (smoothstep falloff, damping along the line between them, capped at 1.5 g). Sticky joints are soft weld constraints, solved as one 3×3 block per weld alongside the contacts. A tipped block is the piece squashed to one cell thick, with its mass unchanged.
  - **Look:** explosive and weird blocks animate their palettes. The fireball reuses the circle's sprite, so arcade needs no new sprite tiles.
  - Magnet forces and welds are split into small pieces like the solver's passes, so a big sticky or magnetic pile slows the simulation down instead of dropping frames. Their maths runs as ARM code from ROM, which keeps 64-bit multiplies to one instruction without using IWRAM.
- **Saves:** the best score for each mode and the last mode picked go to SRAM. An older save keeps its classic best.

## Testing

- `make test` runs `tests/smoke.txt` headless through libmgba and writes screenshots to `build/shots/`. On Ubuntu, install libmgba with `apt install libmgba-dev`.
- Script commands are `wait N`, `hold KEYS`, `tap KEYS [N]`, `shot FILE.ppm`, `audio FILE.wav`, and `mark`.
- `make bench` builds `bench.gba` with a settled 16-block tower. It logs the physics cost per frame to the mGBA debug log.
- For arcade testing, build with `CFLAGS="... -DARCADE_TEST=N"`. With N from 1 to 6, condition N (giant, tiny, weird, magnet storm, sticky, powder keg) is on from the first turn and never ends. With N from 7 to 11, every block gets one special: 7 magnets of either charge, 8 negative magnets, 9 sticky, 10 explosive, 11 weird.
- `tools/hostsim/` compiles the physics engine for the host (`-DHOST`), so you can run quick stacking experiments without an emulator:

```sh
gcc -O2 -DHOST -I src -o seq tools/hostsim/seq.c src/physics.iwram.c src/gen/pieces_gen.c src/gen/tables_gen.c
./seq 30 0.3 1    # drop 30 rectangles alternating ±0.3 cells; reports if one falls
```
