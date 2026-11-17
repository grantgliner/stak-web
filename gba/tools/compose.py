#!/usr/bin/env python3
"""Write the two STAK! GBA tunes as 4-track MIDI (music/<song>.mid: lead,
harmony, bass, drums) for tools/gen_music.py.

These are original arrangements written for the GBA's PSG channels. They take
their key, tempo, chord colour and the shape of the main motifs from the
original songs (tools/stems_to_midi.py transcribes those, for reference):

  menu     ~109 bpm, C major with borrowed Bb and Eb chords (the original's
           bVII / bIII colour), an unhurried melody built on a falling fourth.
  classic   166 bpm, E dorian (Em - C - D - A), driving octave bass and a
           syncopated lead that leans on B and E like the original.

Melodies are written out by hand below, one bar per string, as
pitch:length tokens with lengths in 16th notes (r = rest). Harmony
(broken chords), bass and drums are generated from the chord chart with a
pattern per song. Every bar is checked to add up to 16.

Needs pretty_midi.  python3 tools/compose.py
"""
import os
import pretty_midi

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'music')

PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def pitch(name):
    """'F#5' -> 78, 'Bb4' -> 70 (C4 = 60)."""
    p = PC[name[0]]
    rest = name[1:]
    while rest and rest[0] in '#b':
        p += 1 if rest[0] == '#' else -1
        rest = rest[1:]
    return p + 12 * (int(rest) + 1)


def chord(sym):
    """'Am' -> (root pc, intervals, bass pc). Supports maj, m, 7, m7, sus4, /bass."""
    bass = None
    if '/' in sym:
        sym, b = sym.split('/')
        bass = pitch(b + '0') % 12
    root = pitch((sym[:2] if len(sym) > 1 and sym[1] in '#b' else sym[0]) + '0') % 12
    q = sym[2:] if len(sym) > 1 and sym[1] in '#b' else sym[1:]
    iv = {'': [0, 4, 7], 'm': [0, 3, 7], '7': [0, 4, 7, 10], 'm7': [0, 3, 7, 10],
          'sus4': [0, 5, 7], 'maj7': [0, 4, 7, 11]}[q]
    return root, iv, root if bass is None else bass


def bars_of(lines):
    bars = []
    for i, line in enumerate(lines):
        notes = []
        for tok in line.split():
            n, d = tok.split(':')
            notes.append((0 if n == 'r' else pitch(n), int(d)))
        total = sum(d for _, d in notes)
        assert total == 16, f'bar {i + 1} has {total} sixteenths: {line}'
        bars.append(notes)
    return bars


def in_window(pc, lo):
    """lowest pitch with pitch class pc at or above lo"""
    return lo + (pc - lo) % 12


def harmony(chords, lo, pattern, step):
    """broken chord per bar: voicing root/3rd/5th/(7th or octave) from the root at
    or above lo; pattern indexes the voicing, one entry per `step` sixteenths."""
    out = []
    for sym in chords:
        root, iv, _ = chord(sym)
        r = in_window(root, lo)
        v = [r + i for i in iv[:3]] + [r + (iv[3] if len(iv) > 3 else 12)]
        out.append([(v[k], step) for k in pattern])
    return out


def bass_line(chords, lo, style):
    out = []
    n = len(chords)
    for i, sym in enumerate(chords):
        root, iv, b = chord(sym)
        r = in_window(b, lo)
        fifth = in_window((root + 7) % 12, lo)
        nxt = in_window(chord(chords[(i + 1) % n])[2], lo)
        if style == 'menu':
            # dotted root, a push, the fifth, then a walk into the next chord
            if i % 2 == 0:
                out.append([(r, 6), (r, 2), (fifth, 6), (r + 12, 2)])
            else:
                app = nxt - 1 if nxt - 1 != r else nxt + 2   # chromatic approach from below
                out.append([(r, 6), (r, 2), (fifth, 4), (app, 4)])
        else:
            # driving octave eighths, the fifth near the end of the bar
            out.append([(r, 2), (r + 12, 2)] * 3 + [(fifth, 2), (r + 12, 2)])
    return out


KICK, SNARE, HAT, OHAT = 36, 38, 42, 46


def drums(nbars, style):
    bars = []
    for b in range(nbars):
        h = {}
        if style == 'menu':
            for c in (0, 10): h[c] = KICK
            for c in (4, 12): h[c] = SNARE
            for c in (2, 6, 8, 14): h.setdefault(c, HAT)
            if b % 2 == 1: h[14] = OHAT
            if b % 8 == 7:                       # fill into the next phrase
                h.update({12: SNARE, 14: SNARE, 15: SNARE})
        else:
            for c in (0, 8, 10): h[c] = KICK
            for c in (4, 12): h[c] = SNARE
            for c in (2, 6, 14): h.setdefault(c, HAT)
            if b % 4 == 3: h[7] = KICK
            if b % 8 == 7:
                h.update({10: SNARE, 11: SNARE, 13: SNARE, 14: SNARE, 15: SNARE})
            elif b % 4 == 3:
                h.update({14: SNARE, 15: SNARE})
            if b % 8 == 0: h[14] = OHAT
        bars.append(h)
    return bars


def write(name, bpm, chords, lead, harm, bass, drum_bars):
    n = len(chords)
    assert len(lead) == len(harm) == len(bass) == len(drum_bars) == n, name
    c = 60 / bpm / 4
    pm = pretty_midi.PrettyMIDI(initial_tempo=bpm)
    for tname, prog, bars in (('lead', 80, lead), ('harmony', 81, harm), ('bass', 38, bass)):
        inst = pretty_midi.Instrument(program=prog, name=tname)
        t = 0
        for bar in bars:
            assert sum(d for _, d in bar) == 16
            for p, d in bar:
                if p:
                    inst.notes.append(pretty_midi.Note(96, p, t * c, (t + d) * c))
                t += d
        pm.instruments.append(inst)
    dr = pretty_midi.Instrument(program=0, name='drums', is_drum=True)
    for b, hits in enumerate(drum_bars):
        for cell, p in sorted(hits.items()):
            t = (b * 16 + cell) * c
            dr.notes.append(pretty_midi.Note(100, p, t, t + c / 2))
    pm.instruments.append(dr)
    pm.write(os.path.join(OUT, name + '.mid'))
    print(f'{name}: {n} bars, {n * 16 * c:.1f}s loop')


# ---- menu: C major, relaxed ------------------------------------------------
MENU_A = ['C', 'G/B', 'Bb', 'F', 'C', 'Am', 'Eb', 'G']
MENU_B = ['F', 'G', 'Em', 'Am', 'Dm', 'G7', 'Bb', 'G']
MENU_CHORDS = MENU_A + MENU_A + MENU_B + MENU_A
MENU_LEAD_A = [
    'E5:3 D5:1 C5:4 G4:6 r:2',          # the falling-fourth motif
    'G4:2 A4:2 B4:2 D5:4 B4:2 G4:4',
    'F5:3 D5:1 Bb4:4 F4:6 r:2',         # same motif a step down, on the borrowed Bb
    'F4:2 G4:2 A4:2 C5:4 A4:2 F4:4',
    'E5:3 D5:1 C5:4 G5:4 E5:4',
    'E5:2 C5:2 A4:4 C5:2 E5:6',
    'Eb5:4 G5:2 F5:2 Eb5:4 Bb4:4',      # bIII colour
    'D5:6 B4:2 G4:4 r:4',
]
MENU_LEAD_A2 = [
    'E5:3 D5:1 C5:4 G4:4 C5:2 E5:2',
    'D5:2 B4:2 G4:2 B4:2 D5:4 G5:4',
    'F5:3 D5:1 Bb4:4 F4:4 Bb4:2 D5:2',
    'C5:2 A4:2 F4:2 A4:2 C5:4 F5:4',
    'E5:2 G5:2 E5:2 C5:2 D5:2 E5:2 G5:4',
    'A5:4 G5:2 E5:2 C5:4 E5:4',
    'G5:4 F5:2 Eb5:2 Bb4:2 Eb5:2 F5:4',
    'G5:8 r:4 B4:2 D5:2',
]
MENU_LEAD_B = [
    'F5:4 E5:2 F5:2 A5:4 F5:4',
    'G5:6 D5:2 B4:4 D5:4',
    'E5:4 G5:2 E5:2 B4:4 E5:4',
    'C5:2 B4:2 A4:2 B4:2 C5:4 E5:4',
    'F5:4 E5:2 D5:2 A4:4 D5:4',
    'B4:2 C5:2 D5:2 F5:2 D5:4 B4:4',
    'F5:4 D5:2 F5:2 Bb5:6 A5:2',
    'G5:12 r:4',
]
MENU_LEAD = bars_of(MENU_LEAD_A + MENU_LEAD_A2 + MENU_LEAD_B + MENU_LEAD_A)

# ---- classic: E dorian, driving ---------------------------------------------
CL_A = ['Em', 'C', 'D', 'A', 'Em', 'C', 'D', 'B']
CL_B = ['C', 'D', 'Bm', 'Em', 'C', 'D', 'Em', 'B7']
CL_CHORDS = CL_A + CL_A + CL_B + CL_A
CL_LEAD_A = [
    'B4:2 E5:2 r:2 E5:2 F#5:2 G5:2 F#5:2 E5:2',   # B up to a repeated E, off the beat
    'G5:4 E5:2 C5:2 r:2 B4:2 C5:2 E5:2',
    'F#5:4 D5:2 A4:2 r:2 A4:2 D5:2 F#5:2',
    'E5:6 C#5:2 A4:4 B4:2 C#5:2',                  # the dorian C# on the A chord
    'B4:2 E5:2 r:2 E5:2 F#5:2 G5:2 A5:2 G5:2',
    'E5:4 G5:2 E5:2 C5:4 E5:4',
    'D5:2 E5:2 F#5:2 A5:2 G5:2 F#5:2 E5:2 D5:2',
    'D#5:6 F#5:2 B5:6 r:2',
]
CL_LEAD_A2 = [
    'E5:2 B4:2 E5:2 G5:2 F#5:2 E5:2 D5:2 E5:2',
    'G5:6 E5:2 C5:4 G5:4',
    'F#5:6 A5:2 D5:4 F#5:2 A5:2',
    'E5:2 C#5:2 A4:2 C#5:2 E5:4 A5:4',
    'G5:4 F#5:2 E5:2 B4:4 E5:4',
    'G5:2 A5:2 G5:2 E5:2 C5:4 E5:4',
    'F#5:4 A5:2 F#5:2 D5:2 E5:2 F#5:2 A5:2',
    'B5:8 A5:2 G5:2 F#5:4',
]
CL_LEAD_B = [
    'E5:4 G5:4 E5:2 D5:2 C5:4',
    'D5:4 F#5:4 A5:6 F#5:2',
    'F#5:2 E5:2 D5:4 B4:4 D5:4',
    'E5:12 G5:2 F#5:2',
    'G5:4 E5:2 G5:2 A5:4 G5:4',
    'F#5:4 D5:2 F#5:2 A5:8',
    'G5:4 F#5:2 E5:2 G5:4 B5:4',
    'A5:8 F#5:4 D#5:4',
]
CL_LEAD = bars_of(CL_LEAD_A + CL_LEAD_A2 + CL_LEAD_B + CL_LEAD_A)


def main():
    os.makedirs(OUT, exist_ok=True)
    write('menu', 108.98, MENU_CHORDS, MENU_LEAD,
          harmony(MENU_CHORDS, 48, [0, 2, 1, 2, 3, 2, 1, 2], 2),
          bass_line(MENU_CHORDS, 40, 'menu'), drums(len(MENU_CHORDS), 'menu'))
    write('classic', 166.0, CL_CHORDS, CL_LEAD,
          harmony(CL_CHORDS, 52, [0, 1, 2, 1, 3, 1, 2, 1], 2),
          bass_line(CL_CHORDS, 40, 'classic'), drums(len(CL_CHORDS), 'classic'))


if __name__ == '__main__':
    main()
