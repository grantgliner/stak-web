#pragma once
#include "gba.h"

/* Twin Turbo sound engine.
 *
 * Music is sample based: an 8-voice software mixer feeds Direct Sound A at
 * 18157 Hz (304 samples per frame, exactly one frame of timer ticks), double
 * buffered and restarted from the VBlank interrupt. A sequencer steps through
 * a byte stream of note events at 48 ticks per beat, and ticks fall *inside*
 * the mix buffer (the buffer is mixed in segments between ticks), so the
 * groove is sample accurate instead of snapping to 60 Hz frames.
 *
 * Next to the samples, the PSG plays hi-hats and shakers on the noise channel
 * (it has the full bandwidth that 18 kHz samples lack), an optional square
 * lead on channel 2, and sound effects on channel 1. */

#define MIX_RATE 18157
#define MIX_LEN  304
#define NVOICE   8

typedef struct {
  const s8* data;
  u32 len;          /* samples */
  u32 loop;         /* loop start in samples; == len means one-shot */
  u32 inc60;        /* Q12 step for MIDI note 60 at MIX_RATE */
  u16 atk, dec;     /* envelope rates per tick, on a 0..65535 scale */
  u16 sus, rel;     /* sustain level, release rate */
  u8 vol;           /* 0..64 */
  u8 fixed;         /* 1: ignore note pitch (drums) */
} Instrument;

/* channel kinds */
enum { CH_OFF, CH_SAMPLE, CH_KIT, CH_NOISE, CH_SQUARE };

typedef struct {
  const u8* ev; u32 len;
  u32 loop_at;                 /* byte offset to jump back to at the end */
  u32 spt_q8;                  /* samples per tick, Q8 */
  u8 kind[12];                 /* per channel; channels 0..7 are mixer voices */
  u8 inst[12];                 /* instrument index (sample) or base of kit */
  u8 sq_duty, sq_env;          /* square lead (channel 9) */
} Song;

extern const Instrument instruments[];

void audio_init(void);
void audio_vblank(void);        /* from the VBlank ISR */
void audio_frame(void);         /* once per frame from the main loop: mixes the next buffer */
void audio_play(const Song* s);
void audio_stop(void);
void audio_fade(int frames);    /* fade music out over n frames */
void audio_set_master(int v);   /* 0..16 */

enum { SFX_JUMP, SFX_LAND, SFX_ORB, SFX_HURT, SFX_SELECT, SFX_CONFIRM, SFX_WHOOSH, SFX_BOOST, SFX_DOOR, SFX_COUNT, SFX_GO, SFX_BUMP };
void sfx(int id);
