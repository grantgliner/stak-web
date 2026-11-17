# Twin Turbo for Game Boy Advance

A homebrew GBA game starring That Franco and tripleset, made to celebrate their TURBO EP.

Pick a runner and sprint right through a retro-future city. Jump over the hover cars coming at you and grab orbs. The other runner follows along, Sonic and Tails style. At the end of the road, both of you jump into the Twin Turbo for a three-lap Mode 7 race against three rival cars.

## Controls

| Button | Run stage | Race |
| --- | --- | --- |
| D-pad | Run left and right | Steer |
| A | Jump (hold for higher). Land on a car's roof to bounce off it | Accelerate |
| B | Slide under low cars | Brake |
| L / R | | Boost (one per 8 orbs collected, at least 2) |
| Select | Swap runners mid-run | |
| Start | Pause | |

Getting hit drops your orbs. Getting hit with no orbs costs a life, and you restart at the last checkpoint. Gold chevron pads on the track give a free boost.

Hold L+R while confirming on the character select screen to skip straight to the race.

## Building

You need an `arm-none-eabi` GCC toolchain. devkitARM works, and so does Ubuntu's `gcc-arm-none-eabi` with `libnewlib-arm-none-eabi`.

```sh
make            # builds twinturbo.gba
```

The generated assets in `src/gen/` are committed, so Python is only needed if you change the art or music. `make assets` regenerates them. It needs Python 3 with numpy, Pillow and mido, plus the Inter Display font (`fonts-inter` on Ubuntu) for the title logo.

`tools/gbafix.py` writes the Nintendo logo, fixed byte and checksum into the header, so the ROM boots on real hardware and flash carts.

## How it works

- **Characters** come from `tools/chars.py`. Each 32×48 frame is built from a hand-pixeled 3/4 head (Franco's quiff, beard and roman nose; tripleset's curls and square jaw) on a posed skeleton. The run is an 8-frame cycle built from the classic key poses (contact, down, push-off, flight), with an upright lean, opposing arm swing and a 1px bob. Jumps use separate take-off, apex, fall and landing-squash frames. Every limb, the torso, the fists and the oversized sneakers are drawn with their own outline in depth order. Frames stream into sprite VRAM as they are needed.
- **Run stage** (`src/run.c`, art from `tools/gen_run.py`, level from `tools/gen_level.py`) runs in Mode 0:
  - Four layers: a far skyline, a parallax mid layer streamed by column, the road, and a text HUD. The sky gradient is drawn with HBlank DMA.
  - Each hover car shows a warning before it arrives, then crosses toward you. You can stomp it from above or slide under it.
  - The partner replays your movement from 10 frames earlier.
- **Title, ending and game over** (`src/title.c`, art from `tools/gen_title.py`) use Mode 4 bitmaps. The chrome TWIN TURBO logo is extruded and bevelled in Python, then slammed in with a BG2 affine zoom.
- **Race** (`src/drive.c`, art from `tools/gen_drive.py`) runs in Mode 1:
  - BG2 is a 1024×1024 affine track. Its matrix and origin are rewritten on every scanline by HBlank DMA, from a table built each frame. That table is the Mode 7 floor.
  - The track is a closed loop drawn only with 0°, 45° and 90° edges, which keeps it within the 256-tile affine limit.
  - BG1 is the horizon panorama. It turns with your heading.
  - A window hides the floor above the horizon, and the sky colour changes per line.
  - Rival cars follow waypoints with a little rubber-banding. They are scaled affine sprites, sorted by depth.
- **Sound** (`src/audio.iwram.c`) is an 8-voice software mixer running in IWRAM:
  - It outputs on Direct Sound A at 18 kHz, with PSG noise for the hats and the PSG square for sparkle arps.
  - The sequencer runs at 48 ticks per quarter note and is sample-accurate inside each mix buffer.
  - `tools/gen_samples.py` synthesizes every instrument.
  - `tools/compose.py` writes the three songs. `music/ep_analysis.md` explains how they relate to the TURBO EP.

## Testing

`make test` runs `tests/smoke.txt`, `tests/title.txt` and `tests/drive.txt` headless through libmgba (`apt install libmgba-dev`). Screenshots and audio go to `build/shots/`. Script commands are `wait N`, `hold KEYS`, `tap KEYS [N]`, `shot FILE.ppm` and `audio FILE.wav`.

Building with `-DAUTOPILOT` makes the race drive itself, which is handy for testing a full race and the ending.
