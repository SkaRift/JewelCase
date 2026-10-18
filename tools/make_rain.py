#!/usr/bin/env python3
"""Jewel Case's built-in music: soft rain on concrete with a little wind, made from noise.

Nothing recorded and nothing licensed: every sound here is generated, so it can ship with the
app freely. The track loops seamlessly: the noise is made periodic (shaped in the frequency
domain over exactly one loop), drops that run past the end wrap round to the start, and every
slow change in level repeats a whole number of times per loop.

usage: python3 tools/make_rain.py [out.mp3] [--seconds 120] [--seed 7]
       [--rain 1.0] [--drops 1.0] [--wind 1.0]   (levels: 0 = off, 1 = default)
Needs NumPy and ffmpeg (with libmp3lame). Writes assets-music/music.mp3 by default, which the
build puts next to the app, where Jewel Case plays it when /data/jewelcase/music is empty.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

SR = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def periodic_noise(n, rng, shape):
    """noise exactly periodic over n samples, its spectrum shaped by shape(freqs)"""
    f = np.fft.rfftfreq(n, 1 / SR)
    spec = (rng.standard_normal(f.size) + 1j * rng.standard_normal(f.size)) * shape(f)
    spec[0] = 0
    x = np.fft.irfft(spec, n)
    return x / (np.abs(x).max() + 1e-9)


def band(lo, hi, slope=1.0):
    """a smooth band-pass with gentle skirts, and an extra tilt (1/f**slope)"""
    def shape(f):
        g = 1 / (1 + (lo / np.maximum(f, 1)) ** 4) / (1 + (f / hi) ** 4)
        return g / np.maximum(f, 20) ** (slope / 2)
    return shape


def lfo(n, cycles, phase=0.0):
    """a slow wave with a whole number of cycles in the loop: 0..1"""
    t = np.arange(n) / n
    return 0.5 + 0.5 * np.sin(2 * np.pi * cycles * t + phase)


def add_wrapped(dst, start, sig):
    n = dst.size
    end = start + sig.size
    if end <= n:
        dst[start:end] += sig
    else:
        k = n - start
        dst[start:] += sig[:k]
        dst[:end - n] += sig[k:]


def drop_sound(rng, bright):
    """one drop on concrete: a short tick, then a little splash"""
    ln = int(SR * rng.uniform(0.012, 0.05))
    t = np.arange(ln) / SR
    noise = rng.standard_normal(ln)
    # brighter drops ring higher; a first-difference keeps them crisp
    if bright < 0.5:
        noise = np.convolve(noise, np.ones(4) / 4, 'same')
    else:
        noise = np.diff(noise, prepend=0)
    env = np.exp(-t / rng.uniform(0.003, 0.012))
    tick = noise * env
    # a faint resonant "plip" now and then (water on water)
    if rng.random() < 0.12:
        fr = rng.uniform(1800, 4200)
        tick += 0.6 * np.sin(2 * np.pi * fr * t * (1 + 0.6 * t * 30)) * np.exp(-t / 0.008)
    return tick


def make(seconds, seed, rain, drops, wind):
    n = int(SR * seconds)
    rng = np.random.default_rng(seed)
    out = np.zeros((2, n))

    # the steady hiss of rain: wide, soft, each side its own noise
    for ch in range(2):
        hiss = periodic_noise(n, rng, band(500, 5200, 0.5))
        body = periodic_noise(n, rng, band(450, 1700, 0.6))
        swell = 0.8 + 0.2 * lfo(n, 3, ch * 0.7) * lfo(n, 7, 1.3 + ch)
        out[ch] += rain * (0.30 * hiss + 0.24 * body) * swell

    # individual drops, denser when the rain swells
    dens = 0.6 + 0.4 * lfo(n, 3)
    count = int(seconds * 70 * drops)
    for _ in range(count):
        pos = int(rng.integers(0, n))
        if rng.random() > dens[pos]:
            continue
        near = rng.random() ** 2.2                    # most drops far and quiet
        amp = 0.02 + 0.22 * near
        pan = rng.uniform(-0.9, 0.9)
        s = drop_sound(rng, rng.random()) * amp
        add_wrapped(out[0], pos, s * np.sqrt(0.5 * (1 - pan)))
        add_wrapped(out[1], pos, s * np.sqrt(0.5 * (1 + pan)))

    # wind: low, slow gusts that move across
    if wind > 0:
        gust = lfo(n, 2, 0.4) ** 2 * 0.7 + 0.3 * lfo(n, 5, 2.0)
        for ch in range(2):
            w = periodic_noise(n, rng, band(160, 700, 1.2))
            out[ch] += wind * 0.10 * w * (0.25 + 0.75 * gust * (0.8 + 0.2 * lfo(n, 4, ch * 1.9)))

    # gentle, then even out: quiet enough to sit under the menu
    out = np.tanh(out * 1.4) / np.tanh(1.4)
    out *= 10 ** (-30 / 20) / (np.sqrt((out ** 2).mean()) + 1e-9)   # about -30 dBFS: background
    peak = np.abs(out).max()
    if peak > 0.89:
        out *= 0.89 / peak
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', default=os.path.join(ROOT, 'assets-music', 'music.mp3'))
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--rain', type=float, default=1.0)
    ap.add_argument('--drops', type=float, default=1.0)
    ap.add_argument('--wind', type=float, default=1.0)
    ap.add_argument('--kbps', type=int, default=128)
    a = ap.parse_args()

    audio = make(a.seconds, a.seed, a.rain, a.drops, a.wind)
    pcm = (np.clip(audio.T, -1, 1) * 32767).astype('<i2')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, 'rain.wav')
        with wave.open(wav, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm.tobytes())
        if a.out.endswith('.wav'):
            os.replace(wav, a.out)
        else:
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, '-codec:a', 'libmp3lame',
                            '-b:a', f'{a.kbps}k', '-metadata', 'title=Rain (Jewel Case)',
                            '-metadata', 'artist=SkaRift', a.out], check=True)
    print(a.out, os.path.getsize(a.out) // 1024, 'KB')


if __name__ == '__main__':
    sys.exit(main())
