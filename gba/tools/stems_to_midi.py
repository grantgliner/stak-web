#!/usr/bin/env python3
"""Turn audio/menu.m4a and audio/classic.m4a into clean 4-track MIDI files
(music/<song>.mid: lead, harmony, bass, drums) for tools/gen_music.py.

Pipeline:
  1. Demucs (htdemucs_6s) splits each song into stems.
  2. Spotify basic-pitch turns the bass, guitar and "other" stems into notes.
  3. Drum hits come from onsets in the drum stem, sorted into kick / snare /
     hat by where their energy sits in the spectrum.
  4. Everything is snapped to the song's 16th-note grid and reduced to one
     note at a time per part, because each GBA PSG channel is monophonic.

Only needed when the source audio changes; the MIDI output is committed.
Needs: demucs, torch, basic-pitch (with onnxruntime), librosa, pretty_midi.
  python3 tools/stems_to_midi.py [--work DIR]
"""
import argparse, os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
AUDIO = os.path.join(ROOT, '..', 'audio')
OUT = os.path.join(ROOT, 'music')

# name, bpm, time of the first 16th-note grid line (s), grid cell of the first
# downbeat. Tempo and grid offset were fitted to note and drum onsets.
SONGS = [
    ('menu', 108.98, 0.0138, None),
    ('classic', 166.0, 0.0086, None),
]
DRUM_NOTES = {1: 36, 2: 38, 3: 42}   # GM kick, snare, closed hat


def separate(work, song):
    out = os.path.join(work, 'sep', 'htdemucs_6s', song)
    if os.path.exists(os.path.join(out, 'drums.wav')):
        return out
    wav = os.path.join(work, song + '.wav')
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', os.path.join(AUDIO, song + '.m4a'),
                    '-ar', '44100', '-ac', '2', wav], check=True)
    subprocess.run([sys.executable, '-m', 'demucs', '-n', 'htdemucs_6s', '-o', os.path.join(work, 'sep'), wav], check=True)
    return out


def notes_of(path, **kw):
    from basic_pitch.inference import predict, Model
    from basic_pitch import ICASSP_2022_MODEL_PATH
    global _model
    if '_model' not in globals():
        _model = Model(ICASSP_2022_MODEL_PATH)
    _, _, ev = predict(path, _model, onset_threshold=0.5, frame_threshold=0.3, minimum_note_length=80, **kw)
    return [(s, e, int(p), a) for s, e, p, a, _ in ev]   # start s, end s, pitch, amplitude 0..1


def quantize(notes, t0, c, min_amp):
    q = []
    for s, e, p, a in notes:
        if a < min_amp:
            continue
        cs = int(round((s - t0) / c))
        ce = max(cs + 1, int(round((e - t0) / c)))
        q.append((cs, ce, p, a))
    return q


def mono(q, ncell, pick):
    """One note per cell. pick(candidates) -> chosen (cs, ce, p, a) or None.
    Returns per-cell (pitch, onset) with onset True where a new note starts."""
    by_cell = [[] for _ in range(ncell)]
    for n in q:
        for i in range(max(0, n[0]), min(ncell, n[1])):
            by_cell[i].append(n)
    line, prev = [], None
    for i in range(ncell):
        n = pick(by_cell[i], i, prev)
        if n is None:
            line.append((0, False))
        else:
            line.append((n[2], n is not prev or n[0] == i))
        prev = n
    return line


def pick_lead(cands, i, prev):
    if not cands:
        return None
    # pads hold for bars; prefer notes that are short enough to be melody
    short = [n for n in cands if n[1] - n[0] <= 12] or cands
    best = max(short, key=lambda n: (n[2], n[3]))
    # keep the held note if the new candidate is only a brief blip under it
    if prev in cands and prev[2] >= best[2] - 2:
        return prev
    return best


def fold_octaves(line):
    """basic-pitch and the skyline pick sometimes jump an octave to a harmonic;
    move a note an octave if that lands it much closer to the previous one."""
    out, last = [], None
    for p, onset in line:
        if p and last:
            while p - last > 9 and p - 12 >= 48:
                p -= 12
            while last - p > 9 and p + 12 <= 96:
                p += 12
        if p:
            last = p
        out.append((p, onset))
    return out


def pick_bass(cands, i, prev):
    if not cands:
        return None
    if prev in cands:
        return prev
    return min(cands, key=lambda n: (n[2], -n[3]))


def harmony(guitar_q, lead, ncell):
    by_cell = [[] for _ in range(ncell)]
    for n in guitar_q:
        for i in range(max(0, n[0]), min(ncell, n[1])):
            by_cell[i].append(n)
    line, prev = [], None
    for i in range(ncell):
        lp = lead[i][0]
        c = [n for n in by_cell[i] if not lp or n[2] <= lp - 3]
        if not c:
            line.append((0, False)); prev = None; continue
        n = prev if prev in c else max(c, key=lambda n: n[2])
        line.append((n[2], n is not prev or n[0] == i)); prev = n
    return line


def drums(path, t0, c, ncell):
    import librosa
    y, sr = librosa.load(path, sr=22050, mono=True)
    hop = 256
    env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop)
    on = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=hop, units='samples', backtrack=True)
    S = np.abs(np.fft.rfft(np.stack([np.pad(y, (0, 2048))[o:o + 1024] * np.hanning(1024) for o in on]), axis=1)) ** 2
    f = np.fft.rfftfreq(1024, 1 / sr)
    S, f = S[:, f > 40], f[f > 40]                 # ignore rumble
    cen = (S * f).sum(1) / S.sum(1)                # spectral centroid of each hit
    loud = np.log10(S.sum(1) + 1e-12)
    keep = loud > np.percentile(loud, 10)
    hits = {}
    for o, ce, k in zip(on, cen, keep):
        if not k:
            continue
        # these mixes have no bright cymbals: the backbeat clap/snare is the
        # brightest hit, and the low-mid clicks work best as the PSG hat
        kind = 1 if ce < 160 else 2 if ce >= 800 else 3
        cell = int(round((o / sr - t0) / c))
        if 0 <= cell < ncell:
            hits[cell] = min(hits.get(cell, 9), kind)   # kick wins over snare over hat
    return [hits.get(i, 0) for i in range(ncell)]


def write_midi(path, bpm, parts, c, cell0):
    import pretty_midi
    pm = pretty_midi.PrettyMIDI(initial_tempo=bpm)
    for name, prog, line, is_drum in parts:
        inst = pretty_midi.Instrument(program=prog, name=name, is_drum=is_drum)
        if is_drum:
            for i, d in enumerate(line):
                if d and i >= cell0:
                    t = (i - cell0) * c
                    inst.notes.append(pretty_midi.Note(100, DRUM_NOTES[d], t, t + c / 2))
        else:
            cur = None
            for i in range(cell0, len(line) + 1):
                p, onset = line[i] if i < len(line) else (0, True)
                if cur and (p != cur[0] or onset):
                    inst.notes.append(pretty_midi.Note(96, cur[0], (cur[1] - cell0) * c, (i - cell0) * c))
                    cur = None
                if p and cur is None:
                    cur = (p, i)
        pm.instruments.append(inst)
    pm.write(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', default=os.path.join(ROOT, 'build', 'stems'))
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    for song, bpm, t0, cell0 in SONGS:
        c = 60 / bpm / 4
        d = separate(a.work, song)
        import soundfile as sf
        info = sf.info(os.path.join(d, 'drums.wav'))
        ncell = int((info.duration - t0) / c)
        other = quantize(notes_of(os.path.join(d, 'other.wav'), minimum_frequency=150), t0, c, 0.3)
        gtr = quantize(notes_of(os.path.join(d, 'guitar.wav'), minimum_frequency=80), t0, c, 0.3)
        bass = quantize(notes_of(os.path.join(d, 'bass.wav'), minimum_frequency=30, maximum_frequency=300), t0, c, 0.3)
        lead_l = fold_octaves(mono(other, ncell, pick_lead))
        bass_l = mono(bass, ncell, pick_bass)
        harm_l = harmony(gtr, lead_l, ncell)
        drum_l = drums(os.path.join(d, 'drums.wav'), t0, c, ncell)
        if cell0 is None:
            # downbeat: kick on 1 and snare on 2 and 4 fit best
            k = np.bincount([i % 16 for i, v in enumerate(drum_l) if v == 1], minlength=16)
            sn = np.bincount([i % 16 for i, v in enumerate(drum_l) if v == 2], minlength=16)
            cell0 = int(np.argmax([k[p] + sn[(p + 4) % 16] + sn[(p + 12) % 16] for p in range(16)]))
        write_midi(os.path.join(OUT, song + '.mid'), bpm,
                   [('lead', 80, lead_l, False), ('harmony', 81, harm_l, False),
                    ('bass', 38, bass_l, False), ('drums', 0, drum_l, True)], c, cell0)
        print(f'{song}: {ncell} cells, downbeat at cell {cell0}, '
              f'{sum(o for _, o in lead_l)} lead / {sum(o for _, o in harm_l)} harmony / '
              f'{sum(o for _, o in bass_l)} bass notes, {sum(1 for v in drum_l if v)} drum hits', file=sys.stderr)


if __name__ == '__main__':
    main()
