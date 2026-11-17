# STAK! for Game Boy Advance

A homebrew GBA version of STAK! classic mode: stack blocks on the base as high as you can before one falls off.

## Controls

| Button | Action |
| --- | --- |
| Up / Down | Choose a block in the tray. The first block is selected automatically. |
| A | Pick up the selected block. It appears just above the tower. |
| D-pad | Move the held block. Hold to speed up. |
| L / R | Rotate the held block 45° left or right. |
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

- **Blocks** are pre-rendered from `models/pieces.glb` by `tools/gen_pieces.py`. Each block becomes a 64×64 4bpp sprite at 12 px per cell, with a bevel and outline. The 7 classic colours are palettes, so one sprite serves every colour. Rotation uses affine sprites. The same script extracts each block's convex collision parts and mass properties.
- **Title logo** is an 8bpp background. `tools/gen_gfx.py` builds it along with the font, panels, star field and sky gradient. The sky gradient is drawn with HBlank DMA.
- **Physics** (`src/physics.iwram.c`) is a fixed-point (Q16.16) 2D rigid-body engine that runs from IWRAM as ARM code.
  - 1 cell is 2.5 cm, and gravity is the real 9.81 m/s².
  - Collision uses SAT polygon tests with clipped two-point manifolds.
  - Each frame takes 4 substeps of a sequential-impulse solver, with warm starting and a two-point block solver.
  - Settled blocks sleep and then act as solid ground for blocks placed on top. A static balance check wakes a tower when the combined weight above a block no longer sits over its support, so overhangs still topple. This keeps tall towers steady and keeps the per-frame cost to the few blocks that are moving.
- **Rules** follow the Vision Pro and web classic mode:
  - the piece pool and colours,
  - a base 0.205 m wide,
  - friction of 0.8 and restitution of 0.25,
  - +250 × n points for the nth block,
  - game over when a block falls below the base or more than 0.45 m to either side,
  - the same game-over messages.
- **Sound** is PSG only. Music uses channels 2, 3 and 4 plus a harmony line on channel 1. Sound effects take over channel 1.
  - The menu and classic tunes are original arrangements for the PSG, written in `tools/compose.py`. They borrow the key, tempo, chord colour and motif shapes of `audio/menu.m4a` and `audio/classic.m4a`:
    - menu: C major at about 109 bpm, with the original's borrowed Bb and Eb chords;
    - classic: E dorian at 166 bpm, over Em, C, D and A.
  - The melodies are written out bar by bar. Harmony (broken chords), bass and drums are generated from each song's chord chart.
  - Running `compose.py` writes `music/menu.mid` and `music/classic.mid`, with lead, harmony, bass and drum tracks. `tools/gen_music.py` (part of `make assets`) turns those into note data, and the whole song loops. Per-song voice settings live in `gen_music.py`: duty, envelope, vibrato and bass wave.
  - `tools/stems_to_midi.py` transcribes the original audio (Demucs stems, then basic-pitch) into `music/transcribed/` for reference. It needs demucs, torch, basic-pitch and librosa.
- **Saves:** the best score goes to SRAM.

## Testing

- `make test` runs `tests/smoke.txt` headless through libmgba and writes screenshots to `build/shots/`. On Ubuntu, install libmgba with `apt install libmgba-dev`.
- Script commands are `wait N`, `hold KEYS`, `tap KEYS [N]`, `shot FILE.ppm`, `audio FILE.wav`, and `mark`.
- `make bench` builds `bench.gba` with a settled 16-block tower. It logs the physics cost per frame to the mGBA debug log.
- `tools/hostsim/` compiles the physics engine for the host (`-DHOST`), so you can run quick stacking experiments without an emulator:

```sh
gcc -O2 -DHOST -I src -o seq tools/hostsim/seq.c src/physics.iwram.c src/gen/pieces_gen.c src/gen/tables_gen.c
./seq 30 0.3 1    # drop 30 rectangles alternating ±0.3 cells; reports if one falls
```
