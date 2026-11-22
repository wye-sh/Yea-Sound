#!/usr/bin/env python3
"""Placeholder sounds and chiptune music, made from nothing (no samples, so no licences): the Sound
component's generator. Python 3 standard library only; .ogg output also needs ffmpeg.

  sounds.py list                                   the presets
  sounds.py <preset> <out.wav|.ogg> [--seed N] [--pitch SEMITONES] [--variations N]
  sounds.py music <out.wav|.ogg> [--mood calm|chase|fanfare] [--bpm N] [--bars N] [--seed N]
  sounds.py example <dir>                          the example view's whole set

--variations N writes N files, out_1.wav to out_N.wav, each from its own seed: a sound's variations.
Loops (engine, charge, beacon, hum) repeat seamlessly; an intro ends where its loop starts. Music lasts
exactly its bars at its tempo, so it loops on the beat.
"""
import argparse
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

RATE = 44100
TAU = 2 * math.pi


def write(path, samples, rate=RATE):
    """Writes mono samples (floats, -1..1) as 16-bit WAV, or Ogg Vorbis through ffmpeg for .ogg."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if path.endswith('.ogg'):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            sys.exit('sounds.py: .ogg needs ffmpeg; write .wav instead')
        with tempfile.TemporaryDirectory() as tmp:
            wav = os.path.join(tmp, 'sound.wav')
            write(wav, samples, rate)
            subprocess.run([ffmpeg, '-loglevel', 'error', '-y', '-i', wav, '-c:a', 'libvorbis', '-q:a', '4', path], check=True)
        return
    peak = max(1e-9, max(abs(s) for s in samples))
    scale = 0.89 / peak if peak > 0.89 else 1.0  # never clips
    data = struct.pack(f'<{len(samples)}h', *(int(max(-1.0, min(1.0, s * scale)) * 32767) for s in samples))
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def env(t, length, attack=0.003, release=0.02):
    """A simple envelope: up over `attack`, down over the last `release` seconds."""
    if t < attack:
        return t / attack
    if t > length - release:
        return max(0.0, (length - t) / release)
    return 1.0


def square(phase, duty=0.5):
    return 1.0 if (phase / TAU) % 1.0 < duty else -1.0


def saw(phase):
    return 2.0 * ((phase / TAU) % 1.0) - 1.0


def triangle(phase):
    x = (phase / TAU) % 1.0
    return 4.0 * x - 1.0 if x < 0.5 else 3.0 - 4.0 * x


def lowpass(samples, amount):
    """One-pole low-pass: amount 0 (closed) to 1 (open)."""
    out, y = [], 0.0
    for s in samples:
        y += amount * (s - y)
        out.append(y)
    return out


def sweep(length, f0, f1, wave, duty=0.5, curve=1.0, decay=0.0):
    out, phase, n = [], 0.0, int(length * RATE)
    for i in range(n):
        t = i / RATE
        f = f0 + (f1 - f0) * (t / length) ** curve
        phase += TAU * f / RATE
        a = env(t, length) * (math.exp(-decay * t) if decay else 1.0)
        out.append(a * (square(phase, duty) if wave == 'square' else saw(phase) if wave == 'saw' else math.sin(phase)))
    return out


# ---- Presets: (seed, pitch ratio) -> samples ----

def click(rng, p):
    return [0.6 * s for s in sweep(0.03, 2200 * p, 1800 * p, 'square', 0.5, decay=120)]


def coin(rng, p):
    a = sweep(0.07, 988 * p, 988 * p, 'square', 0.5)
    b = sweep(0.22, 1319 * p, 1319 * p, 'square', 0.5, decay=9)
    return [0.45 * s for s in a + b]


def jump(rng, p):
    return [0.5 * s for s in sweep(0.2, 280 * p, 720 * p, 'square', 0.25, curve=0.6, decay=6)]


def laser(rng, p):
    return [0.5 * s for s in sweep(0.28, 1600 * p, 180 * p, 'saw', curve=0.5, decay=5)]


def step(rng, p):
    n = int(0.11 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    thump = lowpass(noise, 0.06 + 0.04 * rng.random())
    tick = lowpass(noise, 0.5)
    f0 = (70 + 25 * rng.random()) * p
    out = []
    for i in range(n):
        t = i / RATE
        body = math.sin(TAU * f0 * t) * math.exp(-t * 45)
        out.append((2.2 * thump[i] * math.exp(-t * 35) + 0.25 * tick[i] * math.exp(-t * 120) + 0.5 * body) * env(t, n / RATE, 0.001, 0.01))
    return out


def hit(rng, p):
    n = int(0.3 * RATE)
    noise = lowpass([rng.uniform(-1, 1) for _ in range(n)], 0.3)
    return [(noise[i] * math.exp(-i / RATE * 25) + 0.7 * math.sin(TAU * 110 * p * (i / RATE) * (1 - i / n * 0.5)) * math.exp(-i / RATE * 12))
            * env(i / RATE, n / RATE) for i in range(n)]


def explosion(rng, p):
    n = int(1.2 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    low = lowpass(lowpass(noise, 0.04 * p), 0.1)
    return [6.0 * low[i] * math.exp(-i / RATE * 3.2) * env(i / RATE, n / RATE, 0.002, 0.2) for i in range(n)]


def engine_tone(phase, wobble):
    return 0.5 * saw(phase) + 0.3 * square(phase * 0.5, 0.3) + 0.2 * math.sin(phase * 3 + wobble)


def engine(rng, p):
    # A whole number of cycles of the tone (55 Hz) and its wobble (6 Hz) in 1 s: it loops seamlessly.
    f = round(55 * p)
    return [0.5 * engine_tone(TAU * f * (i / RATE), 0.6 * math.sin(TAU * 6 * i / RATE)) for i in range(RATE)]


def engine_start(rng, p):
    # Revs up from 18 Hz to the loop's 55 Hz, ending on a whole cycle so the loop follows without a seam.
    f1, f0, length = round(55 * p), 18 * p, 0.9
    n = int(length * RATE)
    cycles = sum((f0 + (f1 - f0) * (i / n) ** 0.7) / RATE for i in range(n))
    stretch = math.ceil(cycles) / cycles
    out, phase = [], 0.0
    for i in range(n):
        f = (f0 + (f1 - f0) * (i / n) ** 0.7) * stretch
        phase += TAU * f / RATE
        out.append(0.5 * min(1.0, 0.25 + i / n) * engine_tone(phase, 0.0))
    return out


def engine_stop(rng, p):
    f0, length = round(55 * p), 1.1
    n = int(length * RATE)
    out, phase = [], 0.0
    for i in range(n):
        f = f0 * (1 - 0.8 * (i / n) ** 0.8)
        phase += TAU * f / RATE
        out.append(0.5 * (1 - i / n) ** 1.5 * engine_tone(phase, 0.0))
    return out


NOTE = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def charge(rng, p):
    # A rising four-note arpeggio, 1 s, that loops (a charge-up held until released).
    out = []
    for k, m in enumerate((60, 64, 67, 71)):
        out += [0.4 * s for s in sweep(0.25, hz(m) * p, hz(m) * p, 'square', 0.125, decay=4)]
    return out


def charge_end(rng, p):
    out = [0.0] * int(0.9 * RATE)
    for m in (72, 76, 79):
        for i, s in enumerate(sweep(0.9, hz(m) * p, hz(m) * p, 'square', 0.25, decay=4)):
            out[i] += 0.25 * s
    return out


def beacon(rng, p):
    # A beep every half second, 1 s: a source to hear move around (loops).
    out = [0.0] * RATE
    for start in (0, RATE // 2):
        for i, s in enumerate(sweep(0.12, 880 * p, 880 * p, 'sine', decay=18)):
            out[start + i] += 0.6 * s
    return out


def hum(rng, p):
    # A soft drone (ambience), 2 s, looping: whole cycles of every partial.
    n = 2 * RATE
    return [0.25 * (math.sin(TAU * 110 * p * i / RATE) + 0.5 * math.sin(TAU * 165 * p * i / RATE) + 0.3 * math.sin(TAU * 0.5 * i / RATE) *
                    math.sin(TAU * 220 * p * i / RATE)) for i in range(n)]


def babble(rng, p):
    # Voice-like syllables (a pulse train through two moving formants): something on the Voice bus.
    out, phase = [], 0.0
    vowels = [(800, 1200), (400, 2000), (300, 2300), (500, 900), (350, 800)]
    for _ in range(9):
        f1, f2 = rng.choice(vowels)
        length = 0.12 + 0.1 * rng.random()
        n = int(length * RATE)
        pitch = (120 + 30 * rng.random()) * p
        for i in range(n):
            t = i / RATE
            phase += TAU * pitch * (1 + 0.05 * math.sin(TAU * 5 * t)) / RATE
            s = sum(math.sin(phase * h) / h * (math.exp(-((h * pitch - f1) / 180) ** 2) + 0.6 * math.exp(-((h * pitch - f2) / 260) ** 2))
                    for h in range(1, 24))
            out.append(1.5 * s * env(t, length, 0.015, 0.03))
        out += [0.0] * int(0.04 * RATE)
    return out


PRESETS = {
    'click': (click, 'a short interface tick'), 'coin': (coin, 'a two-note pickup'), 'jump': (jump, 'a rising blip'),
    'laser': (laser, 'a falling zap'), 'step': (step, 'a footstep (vary --seed)'), 'hit': (hit, 'an impact (vary --seed)'),
    'explosion': (explosion, 'a rumbling blast'), 'engine': (engine, 'an engine loop, 1 s'),
    'engine_start': (engine_start, "the engine loop's intro"), 'engine_stop': (engine_stop, "the engine loop's outro"),
    'charge': (charge, 'a charge-up loop, 1 s'), 'charge_end': (charge_end, "the charge loop's outro chord"),
    'beacon': (beacon, 'a beeping loop to place in the world'), 'hum': (hum, 'an ambience drone loop'),
    'babble': (babble, 'voice-like syllables'),
}


# ---- Music: chiptune that lasts exactly its bars, so it loops on the beat ----

MOODS = {
    # tempo, root note, chords (scale degrees of a minor or major key), lead style
    'calm': (96, 60, 'major', [(0, 'maj'), (9, 'min'), (5, 'maj'), (7, 'maj')], 'soft'),
    'chase': (140, 57, 'minor', [(0, 'min'), (8, 'maj'), (3, 'maj'), (10, 'maj')], 'drive'),
    'fanfare': (120, 60, 'major', [(0, 'maj')], 'fanfare'),
}
SCALES = {'major': [0, 2, 4, 5, 7, 9, 11], 'minor': [0, 2, 3, 5, 7, 8, 10]}


def music(mood, bpm=None, bars=None, seed=1, rate=32000):
    tempo, root, scale, chords, style = MOODS[mood]
    bpm = bpm or tempo
    bars = bars or (1 if style == 'fanfare' else 8)
    rng = random.Random(seed)
    beat = 60.0 / bpm
    n = int(round(bars * 4 * beat * rate)) if style != 'fanfare' else int(1.6 * rate)
    out = [0.0] * n

    def note(start, length, freq, wave, vol, duty=0.5, decay=0.0):
        a, b = int(start * rate), min(n, int((start + length) * rate))
        phase = 0.0
        for i in range(a, b):
            t = (i - a) / rate
            phase += TAU * freq / rate
            s = square(phase, duty) if wave == 'square' else triangle(phase) if wave == 'triangle' else math.sin(phase)
            out[i] += vol * s * env(t, length, 0.004, min(0.03, length * 0.3)) * (math.exp(-decay * t) if decay else 1.0)

    def noise(start, length, vol, decay, tone=0.0):
        a, b = int(start * rate), min(n, int((start + length) * rate))
        for i in range(a, b):
            t = (i - a) / rate
            s = rng.uniform(-1, 1) + (math.sin(TAU * tone * t * (1 - t * 3)) * 2 if tone else 0)
            out[i] += vol * s * math.exp(-decay * t) * env(t, length, 0.001, 0.005)

    if style == 'fanfare':
        for k, m in enumerate((0, 4, 7, 12)):
            note(k * 0.11, 1.5 - k * 0.11, hz(root + 12 + m), 'square', 0.12, 0.25, decay=1.2)
        note(0.0, 1.5, hz(root - 12), 'triangle', 0.3, decay=1.0)
        noise(0.0, 0.3, 0.15, 8)
        return out, rate
    steps = SCALES[scale]
    for bar in range(bars):
        deg, kind = chords[bar % len(chords)]
        chord = [deg, deg + (4 if kind == 'maj' else 3), deg + 7]
        t0 = bar * 4 * beat
        # Bass: roots (calm on the beat, chase in eighths).
        for k in range(4 if style == 'soft' else 8):
            length = beat if style == 'soft' else beat / 2
            m = root - 24 + chord[0] + (12 if style == 'drive' and k % 2 else 0)
            note(t0 + k * length, length * 0.9, hz(m), 'triangle', 0.35)
        # Arpeggio over the chord.
        sub = 2 if style == 'soft' else 4
        for k in range(4 * sub):
            m = root + chord[k % 3] + (12 if (k // 3) % 2 else 0)
            note(t0 + k * beat / sub, beat / sub * 0.8, hz(m), 'square', 0.05 if style == 'soft' else 0.06, 0.125)
        # Lead: a seeded melody on the scale, resting now and then.
        k = 0.0
        while k < 4:
            length = rng.choice([0.5, 1.0, 1.0, 1.5]) if style == 'soft' else rng.choice([0.25, 0.5, 0.5, 1.0])
            length = min(length, 4 - k)
            if rng.random() > 0.2:
                d = chord[rng.randrange(3)] if rng.random() < 0.6 else rng.choice(steps)
                note(t0 + k * beat, length * beat * 0.9, hz(root + 12 + d), 'triangle' if style == 'soft' else 'square', 0.16, 0.25,
                     decay=1.5 if style == 'soft' else 0)
            k += length
        # Drums.
        for b in range(4):
            tb = t0 + b * beat
            if style == 'soft':
                if b in (0, 2):
                    noise(tb, 0.15, 0.25, 30, tone=70)
                noise(tb + beat / 2, 0.04, 0.05, 80)
            else:
                noise(tb, 0.15, 0.35, 28, tone=60)
                if b in (1, 3):
                    noise(tb, 0.12, 0.2, 20)
                for h in range(2):
                    noise(tb + h * beat / 2 + beat / 4, 0.03, 0.06, 90)
    return out, rate


def example(folder):
    """The Sound component's example view's sounds."""
    one = lambda name, preset, seed=1, pitch=1.0: write(os.path.join(folder, name), PRESETS[preset][0](random.Random(seed), pitch))
    one('click.wav', 'click')
    one('coin.wav', 'coin')
    one('jump.wav', 'jump')
    one('laser.wav', 'laser')
    one('explosion.wav', 'explosion')
    for k in range(1, 5):
        one(f'step_{k}.wav', 'step', seed=k)
    one('engine_start.wav', 'engine_start')
    one('engine.wav', 'engine')
    one('engine_stop.wav', 'engine_stop')
    one('charge.wav', 'charge')
    one('charge_end.wav', 'charge_end')
    one('beacon.wav', 'beacon')
    one('hum.wav', 'hum')
    one('babble.wav', 'babble', seed=3)
    for name, mood in (('calm.ogg', 'calm'), ('chase.ogg', 'chase'), ('win.ogg', 'fanfare')):
        samples, rate = music(mood)
        write(os.path.join(folder, name), samples, rate)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--pitch', type=float, default=0.0, help='semitones')
    ap.add_argument('--variations', type=int, default=0)
    ap.add_argument('--mood', default='calm', choices=sorted(MOODS))
    ap.add_argument('--bpm', type=float)
    ap.add_argument('--bars', type=int)
    a = ap.parse_args()
    if a.what == 'list':
        for k, (_, about) in PRESETS.items():
            print(f'{k:14} {about}')
        print(f'{"music":14} chiptune: --mood {"|".join(sorted(MOODS))}')
        return
    if not a.out:
        ap.error('which file?')
    if a.what == 'example':
        example(a.out)
    elif a.what == 'music':
        samples, rate = music(a.mood, a.bpm, a.bars, a.seed)
        write(a.out, samples, rate)
    elif a.what in PRESETS:
        ratio = 2 ** (a.pitch / 12)
        if a.variations:
            base, ext = os.path.splitext(a.out)
            for k in range(1, a.variations + 1):
                write(f'{base}_{k}{ext}', PRESETS[a.what][0](random.Random(a.seed + k), ratio))
        else:
            write(a.out, PRESETS[a.what][0](random.Random(a.seed), ratio))
    else:
        ap.error(f'no preset {a.what!r} (sounds.py list)')


if __name__ == '__main__':
    main()
