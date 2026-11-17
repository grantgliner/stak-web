#!/usr/bin/env python3
"""Twin Turbo's soundtrack: original tech-house grooves written in the vein of
That Franco & tripleset's TURBO EP (136 bpm F-minor grooves, Fm9 to Dbmaj9
vamps, two-note rhythmic vocal chops; see music/ep_analysis.md).

Everything is composed here as note events, then compiled to the engine's
byte stream (src/gen/music_gen.c) and written out as MIDI (music/*.mid).

Channel map (8 sample voices + 2 PSG):
  0 kick   1 clap/snap/rim   2 congas/tok   3 bass
  4-6 Rhodes chord voices (rootless 9th voicings: 3rd, 7th, 9th)
  7 vocal chop / lead      8 PSG noise hats   9 PSG square sparkle
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import gen_samples as GS

PPQ = 48
S16 = PPQ // 4          # ticks per 16th
BAR = PPQ * 4

KICK, CLAP, RIM, CONGA_HI, CONGA_LO, SNAP, TOK = range(7)
HAT, OHAT, SHAKER, CRASH = range(4)

NOTE = {'C': 0, 'Db': 1, 'D': 2, 'Eb': 3, 'E': 4, 'F': 5, 'Gb': 6, 'G': 7, 'Ab': 8, 'A': 9, 'Bb': 10, 'B': 11}


def n(name):
    """'Eb4' -> MIDI"""
    i = 2 if len(name) > 2 and name[1] in 'b#' else 1
    p = NOTE[name[:i]] if name[1:i] != '#' else NOTE[name[0]] + 1
    return p + 12 * (int(name[i:]) + 1)


class Song:
    def __init__(self, name, bpm, swing=2):
        self.name, self.bpm, self.swing = name, bpm, swing
        self.ev = []           # (tick, prio, ch, note, vel)
        self.loop_tick = 0

    def t16(self, bar, step):
        """tick of 16th step in bar, with swing on the off 16ths"""
        return bar * BAR + step * S16 + (self.swing if step % 2 == 1 else 0)

    def note(self, ch, tick, dur, note, vel):
        self.ev.append((tick, 1, ch, note, vel))
        self.ev.append((tick + max(1, dur), 0, ch, note, 0))

    def hit(self, ch, tick, note, vel):
        self.ev.append((tick, 1, ch, note, vel))

    def compile(self):
        ev = sorted(self.ev, key=lambda e: (e[0], e[1]))
        # drop note-offs that would cut a newer note on the same channel
        last_on = {}
        clean = []
        for e in ev:
            t, pr, ch, nt, v = e
            clean.append(e)
        out = bytearray()
        cur = 0
        loop_at = None
        end = self.length
        for (t, pr, ch, nt, v) in clean:
            if t >= end:
                continue
            if loop_at is None and t >= self.loop_tick:
                self._wait(out, self.loop_tick - cur); cur = self.loop_tick
                loop_at = len(out)
            self._wait(out, t - cur); cur = t
            out += bytes((0x80 | ch, nt & 0x7F, v & 0x7F))
        if loop_at is None:
            loop_at = 0
        self._wait(out, end - cur)
        out.append(0xFF)
        return bytes(out), loop_at

    @staticmethod
    def _wait(out, d):
        while d > 0:
            if d < 0x80:
                out.append(d); d = 0
            else:
                k = min(d, 0xFFFF)
                out += bytes((0xFE, k & 0xFF, k >> 8)); d -= k


# ------------------------------------------------------------------ building blocks
def drums_house(s, bar, kick=True, clap=True, hats=True, shaker=True, perc=0, fill=False):
    for b in range(4):
        if kick:
            s.hit(0, s.t16(bar, b * 4), KICK, 120 if b == 0 else 112)
        if hats:
            s.hit(8, s.t16(bar, b * 4 + 2), OHAT, 86)
        if shaker:
            for k in (1, 3):
                s.hit(8, s.t16(bar, b * 4 + k), SHAKER, 46 if k == 1 else 58)
    if clap:
        s.hit(1, s.t16(bar, 4), CLAP, 100)
        s.hit(1, s.t16(bar, 12), CLAP, 104)
    # tech-house percussion: congas and rims in a 2-bar call/response
    if perc:
        if bar % 2 == 0:
            for st, nn, v in ((3, CONGA_HI, 80), (7, CONGA_LO, 70), (10, CONGA_HI, 74), (14, CONGA_LO, 86)):
                s.hit(2, s.t16(bar, st), nn, v)
            s.hit(1, s.t16(bar, 11), RIM, 70)
        else:
            for st, nn, v in ((2, CONGA_LO, 66), (6, CONGA_HI, 82), (9, CONGA_HI, 60), (11, CONGA_LO, 84), (15, TOK, 64)):
                s.hit(2, s.t16(bar, st), nn, v)
            if perc > 1:
                s.hit(1, s.t16(bar, 7), RIM, 64)
    if fill:
        for st in (13, 14, 15):
            s.hit(1, s.t16(bar, st), SNAP, 60 + st * 2)


def chords(s, bar, voicing, pattern, vel=96, dur=2, top_only=False):
    for i, (st, ln) in enumerate(pattern):
        v = vel - (8 if i % 2 else 0)
        for k, nt in enumerate(voicing):
            if top_only and k < 2:
                continue
            s.note(4 + k, s.t16(bar, st), ln * S16 - 2, nt, v - k * 4)


def bassline(s, bar, root, pattern, vel=110):
    for (st, ln, iv, v) in pattern:
        s.note(3, s.t16(bar, st), ln * S16 - 3, root + iv, int(vel * v / 100))


def chop(s, bar, notes, vel=90):
    """notes: list of (step, len16, midi or None)"""
    for (st, ln, nt) in notes:
        if nt:
            s.note(7, s.t16(bar, st), ln * S16 - 2, nt, vel)


def sparkle(s, bar, tones, vel=80, rate=2, start=0, count=16):
    i = 0
    for st in range(start, start + count, rate):
        s.note(9, s.t16(bar, st), S16 * rate - 3, tones[i % len(tones)], vel)
        i += 1


# ------------------------------------------------------------------ RUN: "TURBO RUN" (F minor, 132 bpm)
def song_run():
    s = Song('run', 132, swing=2)
    F2, Db2, Bb1, Eb2 = n('F2'), n('Db2'), n('Bb1'), n('Eb2')
    V = {   # rootless voicings, smooth voice leading
        'Fm9': [n('Ab3'), n('Eb4'), n('G4')],
        'Dbmaj9': [n('C4'), n('Eb4'), n('F4')],
        'Bbm9': [n('Ab3'), n('C4'), n('Db4')],
        'Eb13': [n('G3'), n('C4'), n('Db4')],
    }
    prog = [('Fm9', F2), ('Fm9', F2), ('Dbmaj9', Db2), ('Dbmaj9', Db2), ('Bbm9', Bb1), ('Bbm9', Bb1), ('Eb13', Eb2), ('Eb13', Eb2)]
    STAB_A = [(0, 2), (3, 2), (6, 2), (10, 2), (13, 2)]
    STAB_B = [(2, 1), (3, 2), (6, 2), (8, 1), (11, 2), (14, 2)]
    # rolling bass that dodges the kick
    BASS_A = [(2, 1, 0, 100), (3, 1, 0, 64), (6, 1, 12, 92), (7, 1, 0, 60), (10, 1, 0, 100), (11, 1, 7, 66), (14, 2, 12, 90)]
    BASS_B = [(2, 1, 0, 100), (3, 1, 0, 64), (6, 1, 12, 92), (8, 1, 0, 70), (10, 1, 0, 100), (12, 1, 10, 74), (13, 1, 12, 80), (14, 1, 7, 86), (15, 1, 3, 70)]
    Eb4, F4, G4, Ab4, C5, Bb3 = n('Eb4'), n('F4'), n('G4'), n('Ab4'), n('C5'), n('Bb3')
    # two-note chop hook (b7 <-> root), answered by a lift to the 9th and b3
    HOOK1 = [(0, 1, F4), (1, 1, F4), (2, 1, Eb4), (4, 1, F4), (5, 1, F4), (6, 1, Eb4), (8, 1, F4), (10, 2, Eb4), (13, 2, C5 - 12)]
    HOOK2 = [(0, 1, F4), (1, 1, F4), (2, 1, Eb4), (4, 1, F4), (5, 1, F4), (6, 2, G4), (9, 1, Ab4), (10, 3, G4), (14, 2, Eb4)]
    HOOK3 = [(0, 2, Eb4), (3, 1, F4), (4, 2, Eb4), (7, 1, F4), (8, 1, F4), (9, 1, Eb4), (10, 2, C5 - 12), (12, 4, Bb3)]
    HOOK4 = [(0, 1, Eb4), (1, 1, F4), (2, 2, G4), (6, 1, F4), (7, 1, Eb4), (8, 4, F4)]
    bar = 0
    # intro: 4 bars drums + bass
    for i in range(4):
        drums_house(s, bar, clap=i >= 2, perc=1 if i >= 2 else 0, fill=i == 3)
        bassline(s, bar, F2, BASS_A if i % 2 == 0 else BASS_B, 100)
        bar += 1
    s.loop_tick = bar * BAR
    # A: chords enter
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, perc=1, fill=i == 7)
        chords(s, bar, V[ch], STAB_A if i % 2 == 0 else STAB_B, 92)
        bassline(s, bar, root, BASS_A if i % 2 == 0 else BASS_B)
        bar += 1
    # B: vocal chop hook
    hooks = [HOOK1, HOOK2, HOOK3, HOOK4] * 2
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, perc=2, fill=i == 7)
        chords(s, bar, V[ch], STAB_A if i % 2 == 0 else STAB_B, 86)
        bassline(s, bar, root, BASS_A if i % 2 == 0 else BASS_B)
        chop(s, bar, hooks[i], 92)
        bar += 1
    # C: hook + PSG sparkle arps (the GBA flavour)
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, perc=2, fill=i == 7)
        chords(s, bar, V[ch], STAB_A if i % 2 == 0 else STAB_B, 84)
        bassline(s, bar, root, BASS_A if i % 2 == 0 else BASS_B)
        chop(s, bar, hooks[i], 88)
        tones = [t + 12 for t in V[ch]] + [V[ch][1] + 24]
        sparkle(s, bar, tones, 70, rate=1 if i % 4 == 3 else 2)
        bar += 1
    # breakdown: no kick, long chords
    for i, (ch, root) in enumerate(prog[:4]):
        drums_house(s, bar, kick=False, clap=i >= 2, hats=False, shaker=True, perc=1)
        chords(s, bar, V[ch], [(0, 6), (6, 4), (10, 6)], 80)
        s.note(3, s.t16(bar, 0), BAR - 6, root, 90)
        if i == 3:
            for st in range(8, 16):
                s.hit(1, s.t16(bar, st), SNAP, 40 + st * 4)
        bar += 1
    s.length = bar * BAR
    return s


# ------------------------------------------------------------------ TITLE: "TURBO LOUNGE" (Ab minor, 118 bpm)
def song_title():
    s = Song('title', 118, swing=3)
    V = {
        'Abm9': [n('B3'), n('Gb4'), n('Bb4')],
        'Dbm9': [n('B3'), n('Eb4'), n('E4')],
        'Emaj9': [n('Eb4'), n('Gb4'), n('Ab4')],
        'Ebm11': [n('Db4'), n('Gb4'), n('Ab4')],
        'Eb7#9': [n('Db4'), n('Gb4'), n('G4')],
    }
    prog = [('Abm9', n('Ab1')), ('Abm9', n('Ab1')), ('Dbm9', n('Db2')), ('Dbm9', n('Db2')),
            ('Emaj9', n('E2')), ('Ebm11', n('Eb2')), ('Dbm9', n('Db2')), ('Eb7#9', n('Eb2'))]
    COMP_A = [(0, 3), (3, 2), (6, 3), (10, 2), (12, 3)]
    COMP_B = [(2, 2), (6, 2), (8, 3), (11, 2), (14, 2)]
    BASS = [(0, 3, 0, 100), (3, 1, 12, 70), (6, 2, 0, 90), (10, 1, 7, 76), (11, 2, 12, 84), (14, 2, 10, 80)]
    mel = [
        [(0, 3, n('Eb5')), (3, 2, n('Db5')), (6, 3, n('B4')), (9, 4, n('Bb4')), (14, 2, n('Ab4'))],
        [(10, 2, n('Bb4')), (12, 2, n('B4')), (14, 2, n('Db5'))],
        [(0, 3, n('E5')), (3, 2, n('Eb5')), (6, 4, n('B4')), (11, 2, n('Db5')), (13, 3, n('B4'))],
        [(0, 6, n('Ab4'))],
        [(0, 2, n('Eb5')), (2, 2, n('Db5')), (4, 4, n('B4')), (10, 2, n('Ab4')), (12, 4, n('B4'))],
        [(0, 6, n('Bb4')), (8, 2, n('Db5')), (10, 2, n('B4')), (12, 4, n('Bb4'))],
        [(0, 3, n('Ab4')), (3, 3, n('B4')), (6, 4, n('Eb5')), (12, 4, n('Db5'))],
        [(0, 4, n('G4')), (4, 2, n('Bb4')), (6, 2, n('Db5')), (8, 6, n('Eb5'))],
    ]
    bar = 0
    for rep in range(3):
        for i, (ch, root) in enumerate(prog):
            first = rep == 0
            drums_house(s, bar, kick=not (first and i < 4), clap=False, hats=not first, shaker=True, perc=1 if rep > 0 else 0, fill=i == 7)
            if not first or i >= 2:
                s.hit(1, s.t16(bar, 4), SNAP, 70); s.hit(1, s.t16(bar, 12), SNAP, 76)
            chords(s, bar, V[ch], COMP_A if i % 2 == 0 else COMP_B, 84 if rep else 76)
            bassline(s, bar, root + 12, BASS, 96)
            if rep >= 1:
                chop(s, bar, mel[i], 86)
            if rep == 2 and i % 2 == 1:
                sparkle(s, bar, [t + 12 for t in V[ch]] + [V[ch][2] + 15], 56, rate=2, start=8, count=8)
            bar += 1
        if rep == 0:
            s.loop_tick = 0
    s.length = bar * BAR
    return s


# ------------------------------------------------------------------ DRIVE: "TURBO DRIVE" (F minor, 136 bpm)
def song_drive():
    """Race theme. Same F-minor world as the run, pushed harder: 136 bpm (the
    EP's FEEL THIS tempo), a saw lead on top and the two-note vocal chop on its
    own voice (channel 2 replaces the congas)."""
    s = Song('drive', 136, swing=1)
    V = {
        'Fm9': [n('Ab3'), n('Eb4'), n('G4')],
        'Dbmaj9': [n('C4'), n('Eb4'), n('F4')],
        'Bbm9': [n('Ab3'), n('C4'), n('Db4')],
        'Eb13': [n('G3'), n('C4'), n('Db4')],
        'Abmaj9': [n('G3'), n('Bb3'), n('C4')],
        'C7#9': [n('Bb3'), n('Eb4'), n('E4')],
    }
    prog = [('Fm9', n('F2')), ('Fm9', n('F2')), ('Dbmaj9', n('Db2')), ('Dbmaj9', n('Db2')),
            ('Bbm9', n('Bb1')), ('Eb13', n('Eb2')), ('Abmaj9', n('Ab1')), ('C7#9', n('C2'))]
    STAB = [(0, 1), (2, 1), (3, 2), (6, 1), (8, 1), (10, 2), (13, 2)]
    STAB2 = [(2, 1), (3, 2), (6, 2), (9, 1), (11, 2), (14, 1)]
    # driving offbeat bass: octave pumps between kicks
    BASS = [(2, 1, 0, 100), (3, 1, 12, 70), (6, 1, 0, 96), (7, 1, 12, 66), (10, 1, 0, 100), (11, 1, 12, 70), (14, 1, 0, 92), (15, 1, 10, 74)]
    BASS2 = [(2, 1, 0, 100), (3, 1, 12, 70), (6, 1, 0, 96), (7, 1, 12, 66), (10, 1, 0, 100), (12, 1, 7, 80), (13, 1, 10, 80), (14, 2, 12, 90)]
    Eb4, F4 = n('Eb4'), n('F4')
    CHOP = [(0, 1, F4), (1, 1, F4), (2, 1, Eb4), (6, 1, F4), (8, 1, F4), (9, 1, F4), (10, 1, Eb4), (14, 2, F4)]
    CHOP2 = [(0, 1, F4), (1, 1, F4), (2, 1, Eb4), (6, 1, F4), (8, 1, Eb4), (10, 2, F4), (13, 1, Eb4), (14, 1, F4)]
    N = n
    LEAD = [
        [(0, 2, N('C5')), (3, 1, N('Eb5')), (4, 3, N('F5')), (7, 1, N('Eb5')), (8, 2, N('C5')), (10, 2, N('Ab4')), (12, 4, N('Bb4'))],
        [(0, 2, N('Ab4')), (2, 2, N('Bb4')), (4, 6, N('C5')), (12, 2, N('Eb5')), (14, 2, N('C5'))],
        [(0, 3, N('F5')), (3, 1, N('Eb5')), (4, 2, N('C5')), (6, 2, N('Ab4')), (8, 4, N('C5')), (12, 2, N('Db5')), (14, 2, N('C5'))],
        [(0, 8, N('Ab4')), (10, 2, N('Bb4')), (12, 2, N('C5')), (14, 2, N('Eb5'))],
        [(0, 3, N('F5')), (3, 3, N('Db5')), (6, 2, N('C5')), (8, 2, N('Bb4')), (10, 2, N('Ab4')), (12, 4, N('F4'))],
        [(0, 2, N('G4')), (2, 2, N('Bb4')), (4, 2, N('Db5')), (6, 2, N('C5')), (8, 6, N('Bb4')), (14, 2, N('G4'))],
        [(0, 3, N('C5')), (3, 3, N('Eb5')), (6, 2, N('G5')), (8, 4, N('F5')), (12, 4, N('Eb5'))],
        [(0, 2, N('G5')), (2, 2, N('F5')), (4, 2, N('Eb5')), (6, 2, N('C5')), (8, 3, N('Bb4')), (11, 1, N('C5')), (12, 4, N('E5'))],
    ]

    def lead(bar, notes, vel, oct=0):
        for (st, ln, nt) in notes:
            s.note(7, s.t16(bar, st), ln * S16 - 2, nt + oct, vel)

    def vox(bar, notes, vel):
        for (st, ln, nt) in notes:
            s.note(2, s.t16(bar, st), ln * S16 - 2, nt, vel)

    def rims(bar):
        for st in (3, 7, 11):
            s.hit(1, s.t16(bar, st), RIM, 58 + st)

    bar = 0
    # intro: kick, bass and the vocal chop counting the race in
    for i in range(4):
        drums_house(s, bar, clap=i >= 2, shaker=True, hats=i >= 1, fill=i == 3)
        bassline(s, bar, n('F2'), BASS if i % 2 == 0 else BASS2, 96)
        vox(bar, CHOP if i % 2 == 0 else CHOP2, 88)
        bar += 1
    s.loop_tick = bar * BAR
    # A: full groove, chords and chop
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, fill=i == 7)
        rims(bar)
        chords(s, bar, V[ch], STAB if i % 2 == 0 else STAB2, 88)
        bassline(s, bar, root, BASS if i % 2 == 0 else BASS2)
        vox(bar, CHOP if i % 2 == 0 else CHOP2, 80)
        bar += 1
    # B: the lead takes the melody
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, fill=i == 7)
        rims(bar)
        chords(s, bar, V[ch], STAB if i % 2 == 0 else STAB2, 80)
        bassline(s, bar, root, BASS if i % 2 == 0 else BASS2)
        lead(bar, LEAD[i], 96)
        if i % 2 == 1:
            vox(bar, CHOP2[:3], 70)
        bar += 1
    # C: melody again with chop answers and square sparkle on top
    for i, (ch, root) in enumerate(prog):
        drums_house(s, bar, fill=i == 7)
        rims(bar)
        chords(s, bar, V[ch], STAB if i % 2 == 0 else STAB2, 80)
        bassline(s, bar, root, BASS if i % 2 == 0 else BASS2)
        lead(bar, LEAD[i], 100)
        vox(bar, [(st, ln, nt) for (st, ln, nt) in CHOP if st >= 8], 74)
        sparkle(s, bar, [t + 12 for t in V[ch]] + [V[ch][1] + 24], 62, rate=1 if i % 4 == 3 else 2)
        bar += 1
    # breakdown: no kick, held chords, chop builds back in
    for i, (ch, root) in enumerate(prog[:4]):
        drums_house(s, bar, kick=False, clap=i >= 2, hats=False, shaker=True)
        chords(s, bar, V[ch], [(0, 6), (6, 4), (10, 6)], 78)
        s.note(3, s.t16(bar, 0), BAR - 6, root, 86)
        vox(bar, CHOP if i % 2 == 0 else CHOP2, 60 + i * 8)
        if i == 3:
            for st in range(8, 16):
                s.hit(1, s.t16(bar, st), SNAP, 40 + st * 4)
        bar += 1
    s.length = bar * BAR
    return s


SONGS = [song_run, song_title, song_drive]
KINDS = {  # channel kinds and instruments per song
    'default': dict(kind=['KIT', 'KIT', 'KIT', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'NOISE', 'SQUARE', 'OFF', 'OFF'],
                    inst=['KICK', 'KICK', 'KICK', 'BASS', 'EP', 'EP', 'EP', 'VOX_OO', 0, 0, 0, 0]),
    'drive': dict(kind=['KIT', 'KIT', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'SAMPLE', 'NOISE', 'SQUARE', 'OFF', 'OFF'],
                  inst=['KICK', 'KICK', 'VOX_AH', 'BASS', 'EP', 'EP', 'EP', 'LEAD', 0, 0, 0, 0]),
}


def write_midi(s, path):
    import mido
    mid = mido.MidiFile(ticks_per_beat=PPQ)
    tracks = {}
    names = ['kick', 'clap', 'perc', 'bass', 'keys1', 'keys2', 'keys3', 'chop', 'hats', 'square']
    for ch in range(10):
        tr = mido.MidiTrack(); mid.tracks.append(tr)
        tr.append(mido.MetaMessage('track_name', name=names[ch], time=0))
        if ch == 0:
            tr.append(mido.MetaMessage('set_tempo', tempo=mido.bpm2tempo(s.bpm), time=0))
        tracks[ch] = (tr, [])
    for (t, pr, ch, nt, v) in s.ev:
        if t < s.length:
            mch = 9 if ch in (0, 1, 2, 8) else ch
            tracks[ch][1].append((t, pr, mido.Message('note_on', channel=mch, note=nt, velocity=v)))
            if ch in (0, 1, 2, 8) and v:
                tracks[ch][1].append((t + 6, 0, mido.Message('note_on', channel=mch, note=nt, velocity=0)))
    for ch, (tr, evs) in tracks.items():
        evs.sort(key=lambda e: (e[0], e[1]))
        cur = 0
        for (t, pr, m) in evs:
            m.time = t - cur; cur = t
            tr.append(m)
    mid.save(path)


def main():
    os.makedirs(os.path.join(ROOT, 'music'), exist_ok=True)
    insts = GS.build()
    names = [i.name.upper() for i in insts]
    with open(os.path.join(ROOT, 'src/gen/music_gen.c'), 'w') as c, open(os.path.join(ROOT, 'src/gen/music_gen.h'), 'w') as h:
        c.write('/* generated by tools/compose.py */\n#include "music_gen.h"\n#include "samples_gen.h"\n\n')
        h.write('/* generated by tools/compose.py */\n#pragma once\n#include "../audio.h"\n\n')
        for fn in SONGS:
            s = fn()
            data, loop_at = s.compile()
            k = KINDS.get(s.name, KINDS['default'])
            spt = GS.SR * 60 / (s.bpm * PPQ)
            c.write('static const u8 ev_%s[%d] = {%s};\n' % (s.name, len(data), ','.join(str(b) for b in data)))
            kinds = ','.join('CH_' + x for x in k['kind'])
            inst = ','.join(('INST_' + x) if isinstance(x, str) else str(x) for x in k['inst'])
            c.write('const Song song_%s = { ev_%s, %d, %d, %d, { %s }, { %s }, %d, %d };\n\n' % (
                s.name, s.name, len(data), loop_at, int(round(spt * 256)), kinds, inst, 2, 0xA2))
            h.write('extern const Song song_%s;\n' % s.name)
            write_midi(s, os.path.join(ROOT, 'music/%s.mid' % s.name))
            print(s.name, 'bars', s.length // BAR, 'bytes', len(data), 'seconds', round(s.length / PPQ * 60 / s.bpm, 1))


if __name__ == '__main__':
    main()
