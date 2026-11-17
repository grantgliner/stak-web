# TURBO EP: what the soundtrack borrows

The game's music is original. It is written to sit in the same world as That Franco & tripleset's TURBO EP (extended mixes), not to copy it. Each track was split into stems with Demucs (htdemucs). Tempo came from the drum stem's onset envelope. The bass line came from pYIN on the bass stem, and harmony from CQT chroma on the "other" stem, measured bar by bar.

| Track | Tempo | Centre | What the harmony does |
| --- | --- | --- | --- |
| LUV GROOVE | ~140 bpm | Ab minor | Abm9 to Dbm9 vamp, minor-9th colour throughout |
| TURBO 96 | 136 bpm | Gb major / Bb minor | Gbmaj7 colour (Gb, Bb, Db, F) over a bass that moves Gb to B |
| FEEL THIS | 136 bpm | F minor | Fm9 vamp lifting to Dbmaj9. The vocal chop is a two-note rhythmic figure on Eb4 and F4 (b7 to root) |
| MISS YOU | ~143.5 bpm | F dorian / Bb major | Bass alternates F and G each bar: Fm9 (Ab, C, Eb over F) to Gm7 (G, Bb, D, F) |

Across the EP: four-on-the-floor tech-house kick, offbeat open hats, shaker 16ths, minor 9th and major 9th chords voiced without the root, a rubbery bass that avoids the kick, and short chopped vocal hooks.

## How the game uses it

- **Run stage, "Turbo Run"** (132 bpm, F minor): the FEEL THIS world. Fm9, Dbmaj9, Bbm9, Eb13. A two-note vocal chop hook (b7 and root) answered by a lift to the 9th. Square-wave sparkle arps in the last section.
- **Title, "Turbo Lounge"** (118 bpm, Ab minor): LUV GROOVE's key and its Abm9 to Dbm9 move, slowed into a lounge groove with a lyrical "oo" vocal melody. The turnaround goes through Emaj9, Ebm11 and Eb7#9.
- **Race, "Turbo Drive"** (136 bpm, F minor): FEEL THIS and TURBO 96 tempo. Fm9, Dbmaj9, Bbm9, Eb13, Abmaj9, C7#9, with a saw lead melody and the Eb/F vocal chop on its own voice.

All instruments are synthesized for the GBA by `tools/gen_samples.py`: FM Rhodes, the bass, formant "ah" and "oo" vocal chops, saw lead, and a kit with its kick tuned to F. `tools/compose.py` writes the songs and saves MIDI copies here (`run.mid`, `title.mid`, `drive.mid`).
