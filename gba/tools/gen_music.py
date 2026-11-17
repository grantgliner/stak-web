#!/usr/bin/env python3
"""Rough automatic transcription of the STAK! menu and classic tracks into
PSG note data for the GBA (src/gen/music_gen.c).

Not a faithful arrangement: it estimates, per 16th note of a known tempo grid,
the most salient melody pitch (after removing sustained pads), the bass pitch
and a kick/snare/hat hit, then loops a section of the song. Needs numpy and
ffmpeg. Tempo/phase values below were found by beat-tracking the files.
"""
import os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIO = os.path.join(HERE, '..', '..', 'audio')
OUT = os.path.join(HERE, '..', 'src', 'gen', 'music_gen.c')
SR = 22050
N = 8192

SONGS = [
    # name, file, bpm, first-beat time (s), cells per beat, first bar, bars, lead octave shift
    ('song_menu', 'menu.m4a', 109.0, 0.516, 4, 0, 32, 0),
    ('song_classic', 'classic.m4a', 166.0, 0.324, 4, 0, 32, 0),
]


def load(path):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR), '-f', 'f32le', '-'],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


FREQ = np.fft.rfftfreq(N, 1 / SR)


def salience(spec, lo, hi, nh):
    out = np.zeros(hi - lo + 1)
    for i, m in enumerate(range(lo, hi + 1)):
        f0 = 440 * 2 ** ((m - 69) / 12)
        for h in range(1, nh + 1):
            fh = f0 * h
            if fh > SR / 2 - 100:
                break
            a = np.searchsorted(FREQ, fh * 2 ** (-1 / 36))
            b = np.searchsorted(FREQ, fh * 2 ** (1 / 36)) + 1
            out[i] += spec[a:b].max() * 0.85 ** (h - 1)
    return out


def band(spec, lo, hi):
    return spec[np.searchsorted(FREQ, lo):np.searchsorted(FREQ, hi)].sum()


def transcribe(x, bpm, phase, div, bar0, bars):
    cell = 60 / bpm / div
    k0 = bar0 * 4 * div
    ncell = bars * 4 * div
    win = np.hanning(N)
    spec = []
    for k in range(k0 - 8, k0 + ncell + 8):
        c = int((phase + (k + 0.5) * cell) * SR)
        seg = x[max(0, c - N // 2):max(0, c + N // 2)]
        seg = np.pad(seg, (0, N - len(seg)))
        spec.append(np.abs(np.fft.rfft(seg * win)))
    spec = np.array(spec)
    # onset spectra (short window at the start of each cell) for drums
    n2 = 1024
    w2 = np.hanning(n2)
    f2 = np.fft.rfftfreq(n2, 1 / SR)
    def short(t):
        c = int(t * SR)
        seg = x[max(0, c):c + n2]
        seg = np.pad(seg, (0, n2 - len(seg)))
        return np.abs(np.fft.rfft(seg * w2))
    def bsum(s, lo, hi):
        return s[np.searchsorted(f2, lo):np.searchsorted(f2, hi)].sum()
    lead, bass, pad, conf, energy, kick, snare, hat = [], [], [], [], [], [], [], []
    for i in range(8, 8 + ncell):
        med = np.median(spec[i - 8:i + 9], axis=0)
        res = np.maximum(spec[i] - 0.7 * med, 0)
        sm = salience(res, 62, 93, 6)
        sb = salience(spec[i], 28, 54, 4)
        lead.append(62 + int(sm.argmax()))
        conf.append(sm.max() / (np.median(sm) + 1e-9))
        bass.append(28 + int(sb.argmax()))
        pad.append(55 + int(salience(spec[i], 55, 76, 5).argmax()))
        energy.append(spec[i].sum())
        t = phase + (k0 + i - 8) * cell
        a, b = short(t - 0.03), short(t + 0.01)
        d = np.maximum(b - a, 0)
        kick.append(bsum(d, 40, 140))
        snare.append(bsum(d, 1500, 5000))
        hat.append(bsum(d, 7000, 11000))
    conf, energy = np.array(conf), np.array(energy)
    kick, snare, hat = np.array(kick), np.array(snare), np.array(hat)
    lead = [m if conf[i] > 3.0 and energy[i] > 0.2 * np.median(energy) else 0 for i, m in enumerate(lead)]
    drums = []
    for i in range(ncell):
        if kick[i] > np.percentile(kick, 75) and i % 2 == 0:
            drums.append(1)
        elif snare[i] > np.percentile(snare, 82):
            drums.append(2)
        elif hat[i] > np.percentile(hat, 55) and i % 2 == 0:
            drums.append(3)
        else:
            drums.append(0)
    # bass: one note per 8th
    bass8 = []
    for i in range(0, ncell, 2):
        bass8 += [bass[i]] * 2
    # pad / harmony: most common sustained pitch per beat, on the second square channel
    harm = []
    for i in range(0, ncell, div):
        vals, cnt = np.unique(pad[i:i + div], return_counts=True)
        harm += [int(vals[cnt.argmax()])] * div
    return lead, bass8, drums, harm, cell


def to_events(notes, cell, div, retrigger_beat=True, merge=True):
    """(note, frames) pairs; equal neighbours merge, but re-trigger on each beat."""
    fpc = cell * 60
    ev = []
    for i, n in enumerate(notes):
        start, end = round(i * fpc), round((i + 1) * fpc)
        dur = end - start
        if ev and merge and ev[-1][0] == n and not (retrigger_beat and i % div == 0) and ev[-1][1] + dur < 255:
            ev[-1][1] += dur
        else:
            ev.append([n, dur])
    return ev


def drum_events(drums, cell):
    fpc = cell * 60
    ev = []
    for i, d in enumerate(drums):
        dur = round((i + 1) * fpc) - round(i * fpc)
        if ev and d == 0 and ev[-1][1] + dur < 255:
            ev[-1][1] += dur
        else:
            ev.append([d, dur])
    return ev


def c_array(name, ev):
    flat = [v for e in ev for v in e]
    lines = []
    for i in range(0, len(flat), 24):
        lines.append('  ' + ', '.join(str(v) for v in flat[i:i + 24]) + ',')
    return f'static const u8 {name}[] = {{\n' + '\n'.join(lines) + '\n};\n'


def main():
    out = ['/* generated by tools/gen_music.py -- rough transcription of audio/menu.m4a and audio/classic.m4a */',
           '#include "music_gen.h"', '']
    for name, fn, bpm, ph, div, bar0, bars, octs in SONGS:
        x = load(os.path.join(AUDIO, fn))
        lead, bass, drums, harm, cell = transcribe(x, bpm, ph, div, bar0, bars)
        lead = [m + 12 * octs if m else 0 for m in lead]
        le = to_events(lead, cell, div)
        be = to_events(bass, cell, div)
        de = drum_events(drums, cell)
        he = to_events(harm, cell, div, retrigger_beat=False)
        p = name.replace('song_', '')
        out.append(c_array(p + '_lead', le))
        out.append(c_array(p + '_bass', be))
        out.append(c_array(p + '_drums', de))
        out.append(c_array(p + '_harm', he))
        out.append(f'const Song {name} = {{ {p}_lead, sizeof {p}_lead, {p}_harm, sizeof {p}_harm, {p}_bass, sizeof {p}_bass, '
                   f'{p}_drums, sizeof {p}_drums, 1, 2, 0x92, 1 }};\n')
        print(f'{name}: {len(le)} lead, {len(be)} bass, {len(de)} drum events, '
              f'{sum(e[1] for e in le) / 60:.1f}s loop', file=sys.stderr)
    open(OUT, 'w').write('\n'.join(out))


if __name__ == '__main__':
    main()
