#!/usr/bin/env python3
"""Synthesize Twin Turbo's instrument samples (8-bit signed, 18157 Hz) and
describe them for the sound engine.

Pitched instruments are generated at a frequency whose period is a whole
number of samples, so their sustain loop is a single clean cycle; the exact
tuning is folded into each instrument's step value (inc60). The engine's
envelope does the volume shaping; the samples carry only the timbre and the
attack transient."""
import numpy as np

SR = 18157
rng = np.random.default_rng(7)


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def lowpass(x, fc):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1 / np.sqrt(1 + (f / fc) ** 4)
    return np.fft.irfft(X, len(x))


def bandpass(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= (1 / np.sqrt(1 + (lo / np.maximum(f, 1)) ** 4)) * (1 / np.sqrt(1 + (f / hi) ** 4))
    return np.fft.irfft(X, len(x))


def norm(x, peak=0.97):
    m = np.max(np.abs(x))
    return x / m * peak if m > 0 else x


class Inst:
    def __init__(self, name, data, loop, inc60, atk, dec, sus, rel, vol, fixed=0):
        self.name, self.data, self.loop = name, data, loop
        self.inc60, self.atk, self.dec, self.sus, self.rel, self.vol, self.fixed = inc60, atk, dec, sus, rel, vol, fixed


def to_s8(x):
    return np.clip(np.round(x * 127), -128, 127).astype(np.int8)


def pitched(root, gen, attack_s, period_mult=1):
    """gen(t, f) -> signal; must be exactly periodic in 1/f after attack_s.
    Returns (s8 data, loop start, inc60)."""
    f_true = midi_hz(root)
    P = int(round(SR / f_true))
    f = SR / P
    n_att = int(round(attack_s * SR / P)) * P
    n = n_att + P * period_mult
    t = np.arange(n) / SR
    x = norm(gen(t, f))
    inc60 = int(round(4096 * (midi_hz(60) / f)))
    return to_s8(x), n_att, inc60


def ms(rate_ticks):
    """envelope rate that takes `rate_ticks` ticks (~10 ms) to cover full scale"""
    return max(1, int(65535 / max(1, rate_ticks)))


# ------------------------------------------------------------------ pitched
def ep(t, f):
    # tine electric piano: 2-op FM, mod index falls from bright to mellow,
    # plus a short metallic tine partial and a little 2nd harmonic "bark"
    I = 0.28 + 1.9 * np.exp(-t / 0.09)
    I[t > 0.5] = 0.28
    y = np.sin(2 * np.pi * f * t + I * np.sin(2 * np.pi * f * t))
    tine = np.sin(2 * np.pi * 7 * f * t) * 0.35 * np.exp(-t / 0.025)
    bark = np.sin(2 * np.pi * 2 * f * t) * 0.25 * np.exp(-t / 0.12)
    bark[t > 0.5] = 0
    y = y + tine + bark
    return np.tanh(1.3 * y)


def bass(t, f):
    # rubbery tech-house bass: strong fundamental, saw harmonics behind a
    # low-pass that snaps shut over ~90 ms
    y = 1.0 * np.sin(2 * np.pi * f * t)
    cut = 3.2 + 9 * np.exp(-t / 0.045)
    cut[t > 0.3] = 3.2
    for k in range(2, 16):
        g = 1 / k / (1 + (k / cut) ** 4)
        y += g * np.sin(2 * np.pi * k * f * t) * 0.9
    return np.tanh(1.6 * y)


def vox(formants):
    def g(t, f):
        y = np.zeros_like(t)
        for k in range(1, int(4200 / f)):
            fk = k * f
            a = 0
            for (F, B, A) in formants:
                a += A / (1 + ((fk - F) / B) ** 2)
            y += a / k ** 0.4 * np.sin(2 * np.pi * fk * t + k * 0.7)
        # breathy onset
        n = rng.standard_normal(len(t)) * 0.25 * np.exp(-t / 0.03)
        return y / np.max(np.abs(y)) + bandpass(n, 1500, 5000)
    return g


def organ(t, f):
    y = np.zeros_like(t)
    for k, a in ((1, 1.0), (2, 0.7), (3, 0.45), (4, 0.4), (6, 0.2), (8, 0.18)):
        y += a * np.sin(2 * np.pi * k * f * t)
    click = rng.standard_normal(len(t)) * np.exp(-t / 0.004) * 0.6
    click[t > 0.05] = 0
    return y + click


def lead(t, f):
    # soft saw lead: band-limited saw, slightly hollowed (pulse-ish)
    y = np.zeros_like(t)
    for k in range(1, int(5000 / f)):
        a = 1 / k * (0.55 if k % 2 == 0 else 1.0) / (1 + (k / 9) ** 2)
        y += a * np.sin(2 * np.pi * k * f * t)
    return y


def bell(t, f):
    I = 0.6 + 2.5 * np.exp(-t / 0.05)
    I[t > 0.3] = 0.6
    return np.sin(2 * np.pi * f * t + I * np.sin(2 * np.pi * 3 * f * t))


# ------------------------------------------------------------------ drums
def kick():
    t = np.arange(int(0.32 * SR)) / SR
    f = 43.65 + 120 * np.exp(-t / 0.028) + 30 * np.exp(-t / 0.004)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) * np.exp(-t / 0.22) * np.minimum(1, (0.32 - t) / 0.03)
    y += rng.standard_normal(len(t)) * np.exp(-t / 0.002) * 0.4
    return np.tanh(1.8 * y)


def clap():
    t = np.arange(int(0.26 * SR)) / SR
    env = np.zeros_like(t)
    for k, d in enumerate((0.0, 0.009, 0.019)):
        m = t >= d
        env[m] += np.exp(-(t[m] - d) / 0.004) * (0.8 if k < 2 else 0)
    m = t >= 0.026
    env[m] += np.exp(-(t[m] - 0.026) / 0.07)
    n = bandpass(rng.standard_normal(len(t)), 800, 2600)
    return n / np.max(np.abs(n)) * env


def rim():
    t = np.arange(int(0.07 * SR)) / SR
    y = np.sin(2 * np.pi * 1650 * t) * np.exp(-t / 0.012) + 0.7 * np.sin(2 * np.pi * 520 * t) * np.exp(-t / 0.018)
    y += rng.standard_normal(len(t)) * np.exp(-t / 0.002) * 0.5
    return y


def conga(f0, dec):
    t = np.arange(int(0.25 * SR)) / SR
    f = f0 * (1 + 0.25 * np.exp(-t / 0.01))
    y = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / dec)
    y += bandpass(rng.standard_normal(len(t)), 1500, 4000) * np.exp(-t / 0.004) * 0.6
    return y


def snap():
    t = np.arange(int(0.12 * SR)) / SR
    n = bandpass(rng.standard_normal(len(t)), 1800, 5000)
    return n / np.max(np.abs(n)) * np.exp(-t / 0.025) + np.sin(2 * np.pi * 900 * t) * np.exp(-t / 0.01) * 0.4


def tok():
    t = np.arange(int(0.08 * SR)) / SR
    return np.sin(2 * np.pi * 1180 * t) * np.exp(-t / 0.02) + np.sin(2 * np.pi * 2100 * t) * np.exp(-t / 0.008) * 0.4


def oneshot(name, x, vol=64):
    d = to_s8(norm(x))
    return Inst(name, d, len(d), 4096, 65535, 0, 65535, ms(3), vol, fixed=1)


def build():
    insts = []
    d, lp, inc = pitched(60, ep, 0.55, 4)
    insts.append(Inst('ep', d, lp, inc, 65535, ms(320), 0, ms(14), 46))
    d, lp, inc = pitched(36, bass, 0.30, 1)
    insts.append(Inst('bass', d, lp, inc, 65535, ms(60), 26000, ms(5), 64))
    d, lp, inc = pitched(69, vox([(780, 110, 1.0), (1180, 130, 0.7), (2600, 220, 0.25)]), 0.08, 2)
    insts.append(Inst('vox_ah', d, lp, inc, ms(2), ms(80), 30000, ms(9), 44))
    d, lp, inc = pitched(69, vox([(360, 80, 1.0), (850, 120, 0.45), (2400, 220, 0.12)]), 0.08, 2)
    insts.append(Inst('vox_oo', d, lp, inc, ms(3), ms(90), 32000, ms(10), 46))
    d, lp, inc = pitched(60, organ, 0.06, 2)
    insts.append(Inst('organ', d, lp, inc, 65535, ms(40), 12000, ms(6), 34))
    d, lp, inc = pitched(72, lead, 0.0, 2)
    insts.append(Inst('lead', d, lp, inc, ms(2), ms(120), 38000, ms(10), 34))
    d, lp, inc = pitched(72, bell, 0.32, 2)
    insts.append(Inst('bell', d, lp, inc, 65535, ms(70), 0, ms(10), 34))
    insts.append(Inst('pad', *pitched(60, lead, 0.0, 2), ms(25), ms(400), 40000, ms(40), 24))
    # kit (order = kit note numbers)
    insts.append(oneshot('kick', kick(), 64))
    insts.append(oneshot('clap', clap(), 40))
    insts.append(oneshot('rim', rim(), 26))
    insts.append(oneshot('conga_hi', conga(340, 0.09), 30))
    insts.append(oneshot('conga_lo', conga(225, 0.12), 32))
    insts.append(oneshot('snap', snap(), 30))
    insts.append(oneshot('tok', tok(), 24))
    return insts


INST = {}
KIT_BASE = 8
KIT = {'kick': 0, 'clap': 1, 'rim': 2, 'conga_hi': 3, 'conga_lo': 4, 'snap': 5, 'tok': 6}


def emit_c(insts, path_c, path_h):
    with open(path_c, 'w') as f:
        f.write('/* generated by tools/gen_samples.py */\n#include "../audio.h"\n\n')
        for i, ins in enumerate(insts):
            f.write('static const s8 smp_%s[%d] __attribute__((aligned(4))) = {' % (ins.name, len(ins.data)))
            f.write(','.join(str(int(v)) for v in ins.data))
            f.write('};\n')
        f.write('\nconst Instrument instruments[] = {\n')
        for ins in insts:
            f.write('  { smp_%s, %d, %d, %d, %d, %d, %d, %d, %d, %d },\n' % (
                ins.name, len(ins.data), ins.loop, ins.inc60, min(65535, ins.atk), ins.dec, ins.sus, ins.rel, ins.vol, ins.fixed))
        f.write('};\n')
    with open(path_h, 'w') as f:
        f.write('/* generated by tools/gen_samples.py */\n#pragma once\n')
        for i, ins in enumerate(insts):
            f.write('#define INST_%s %d\n' % (ins.name.upper(), i))


if __name__ == '__main__':
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, '..'))
    insts = build()
    emit_c(insts, os.path.join(root, 'src/gen/samples_gen.c'), os.path.join(root, 'src/gen/samples_gen.h'))
    print('samples:', sum(len(i.data) for i in insts), 'bytes')
