#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""SlopFin - check captured audio actually sounds like audio.

Silence, clipping, a stuck channel and dropouts are all invisible from a
screenshot and obvious in the samples.
"""

import sys
import wave

import numpy as np

def longest_quiet_run(quiet):
    longest = run = 0
    for value in quiet:
        run = run + 1 if value else 0
        longest = max(longest, run)
    return longest


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/slopfin-audio.wav"
    with wave.open(path, "rb") as handle:
        rate = handle.getframerate()
        frames = handle.getnframes()
        channels = handle.getnchannels()
        data = np.frombuffer(handle.readframes(frames), dtype=np.int16)
    if frames == 0:
        print("no audio captured")
        return 1

    audio = data.reshape(-1, channels).astype(np.float32) / 32768.0
    left, right = audio[:, 0], audio[:, 1 if channels > 1 else 0]
    seconds = frames / rate

    # Eight-channel output is in the standard order, so name the speakers.
    names = (["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"]
             if channels == 8 else [f"ch{i}" for i in range(channels)])

    def rms_db(x):
        value = float(np.sqrt(np.mean(x * x)))
        return -120.0 if value <= 1e-9 else 20.0 * np.log10(value)

    print(f"{seconds:.2f}s, {frames} frames at {rate} Hz, {channels} channels")
    if channels > 2:
        print("  per speaker:")
        for index in range(channels):
            level = rms_db(audio[:, index])
            state = "silent" if level < -70 else "active"
            print(f"    {names[index]:4s} {level:6.1f} dBFS  {state}")
    print(f"  level        left {rms_db(left):6.1f} dBFS   right {rms_db(right):6.1f} dBFS")
    print(f"  peak         left {20*np.log10(max(1e-9, float(np.max(np.abs(left))))):6.1f} dBFS"
          f"   right {20*np.log10(max(1e-9, float(np.max(np.abs(right))))):6.1f} dBFS")
    print(f"  clipped      {int(np.sum(np.abs(audio) >= 0.999))} samples")
    print(f"  identical    {'yes, channels are the same' if np.array_equal(left, right) else 'no, distinct channels'}")

    # Silence long enough to hear as a dropout.
    window = max(1, rate // 100)
    energy = np.abs(audio).sum(axis=1)
    blocks = energy[: len(energy) // window * window].reshape(-1, window).max(axis=1)
    quiet = blocks < 1e-4
    longest = longest_quiet_run(quiet)
    print(f"  silent       {100.0 * float(np.mean(quiet)):.1f}% of blocks, "
          f"longest gap {longest * window / rate * 1000:.0f} ms")

    # A hard discontinuity between neighbouring samples is a join or a dropout.
    jumps = int(np.sum(np.abs(np.diff(left)) > 0.5))
    print(f"  glitches     {jumps} sample-to-sample jumps over half full scale")

    verdict = "signal present; speaker routing requires a known fixture"
    if rms_db(left) < -70 and rms_db(right) < -70:
        verdict = "essentially silent"
    elif longest * window / rate > 0.25:
        verdict = "has audible dropouts"
    elif jumps > frames // 1000:
        verdict = "has frequent discontinuities"
    print(f"\n  verdict: {verdict}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
