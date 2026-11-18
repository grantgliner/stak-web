# STAK! for N64

Classic mode of STAK!, as a homebrew Nintendo 64 ROM. It looks and plays like
the web version. Three blocks lie in a tray in front of the base. A gloved hand
picks one up, the block swings upright above the tower, and you steer it, turn
it and let go. The tower runs on the GBA port's 2D physics engine
(`../gba/src/physics.iwram.c`, compiled with `-DHOST -DN64_PORT`).

## Controls

| N64 | |
| --- | --- |
| Stick (or D-pad) | Move the glove over the tray; steer the held block (the D-pad hops between tray blocks) |
| A | Grab / drop |
| B | Put the block back |
| Z or L / R | Rotate 45° left / right |
| C buttons | Swing the camera a little |
| Start | Pause (B quits to the title) |

The best score is saved to EEPROM 4K.

## How it is built

The ROM targets the browser emulator in the GG Pocket launcher: EmulatorJS's
`mupen64plus_next` core. That core has only a high-level RSP, and its video
plugin (GLideN64) does not draw homebrew RDP triangles. So the game uses
neither the RSP nor the RDP:

- `src/render.c` is a CPU software renderer. It draws flat-lit triangles into
  the 320x240 RGBA5551 frame buffer, with a 16-bit 1/z depth buffer. Whatever
  rarely changes (the HUD, and the base and settled blocks while the camera is
  still) is baked into the background once. Each frame then restores and
  redraws only the region the last frame touched. The glove is a sprite
  rendered from its model at start-up.
- `src/audio.c` is a small synth with square, triangle and noise voices. It
  feeds a polled AI driver; libdragon's interrupt-driven audio driver hangs
  this emulator.
- `src/game.c` is the game, written against an `Input` struct, so it runs in
  the ROM (`src/main.c`) and on a PC (`tools/host/`).

Speed in the emulator: the game logic ticks at 60 Hz. Rendering takes about
20 ms a frame on the core's default settings, which gives a steady 30 fps. With
the core option `mupen64plus-CountPerOp` set to `1`, it runs at 60 fps. Real
hardware runs faster than the emulator's default CPU timing.

libdragon (preview branch) needs a few fixes to run under that core. They are in
`libdragon/stak64-emulator-fixes.patch`:

- The IPL3 verifies RDRAM size.
- No EMUX opcodes.
- KSEG1 instead of XKPHYS for uncached I/O.
- No 64-bit LL/SC atomics.
- A software fallback for MI repeat-mode memset.

The patched IPL3 is checked in as `libdragon/ipl3_dev.z64`.

## Building

1. Install the libdragon toolchain into `$N64_INST`.
2. Run `libdragon/setup.sh`. It clones libdragon, applies the patch, and installs it.
3. Run `make`. The output is `stak64.z64`.

`make assets` regenerates `src/gen/` (meshes, fonts, logo) from the web
version's models. It needs Python with numpy, shapely, mapbox_earcut and Pillow.
The font is Nunito Black (OFL, `tools/Nunito-OFL.txt`).

Build options go in `EXTRA`:

- `make EXTRA=-DDEMO` plays a scripted demo, for testing without a controller.
- `make EXTRA=-DPROFILE` prints frame timings to the IS-Viewer debug output.

Desktop harness: `tools/host/build.sh` builds it. Then
`tools/host/stakhost tools/host/s1.txt` plays the scripted input in that file
and writes the frames to `tools/host/out/`.
