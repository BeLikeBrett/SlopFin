#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""SlopFin - summarise a frame-timing trace.

Judder is a property of the distribution of frame intervals, so this reports
percentiles and the cadence histogram rather than an average.
"""

import csv
import statistics as st
import sys
from collections import Counter


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/slopfin-frames.csv"
    rows = list(csv.DictReader(open(path)))
    if len(rows) < 2:
        print("not enough samples")
        return 1
    num = lambda row, key: int(row[key])

    print(f"{len(rows)} render iterations from {path}\n")
    for key in ("loop_us", "draw_us", "present_us", "decode_us", "convert_us",
                "network_us", "slotwait_us", "pad_us"):
        if key not in rows[0]:
            continue
        values = [num(r, key) for r in rows[1:]]
        ordered = sorted(values)
        pick = lambda f: ordered[min(len(ordered) - 1, int(len(ordered) * f))]
        print(f"  {key[:-3]:10s} median {st.median(values):7.0f}  p95 {pick(0.95):8.0f}"
              f"  p99 {pick(0.99):8.0f}  max {max(values):8.0f} us")

    seconds = (num(rows[-1], "at_us") - num(rows[0], "at_us")) / 1000000
    if seconds > 0 and "decoded" in rows[0]:
        print("\n  stage counters over %.2f seconds:" % seconds)
        for key in ("decoded", "converted", "shown", "dropped", "decode_failures"):
            count = num(rows[-1], key) - num(rows[0], key)
            print(f"    {key:16s} {count:6d}  ({count / seconds:.2f}/s)")

    if seconds > 0 and "audio_underrun" in rows[0]:
        print("\n  audio output in this window:")
        for key in ("audio_played", "audio_underrun", "audio_errors"):
            count = num(rows[-1], key) - num(rows[0], key)
            suffix = f" ({count / 48000:.3f} seconds)" if key != "audio_errors" else ""
            print(f"    {key:16s} {count:7d}{suffix}")
        if 'audio_buffering' in rows[0]:
            waited = num(rows[-1], 'audio_buffering') - num(rows[0], 'audio_buffering')
            events = num(rows[-1], 'audio_rebuffers') - num(rows[0], 'audio_rebuffers')
            print(f"    intentional buffering {waited / 48000:.3f} seconds; {events} rebuffer events")
        for stage, name in ((1, "socket read"), (2, "demux"), (3, "audio submit"), (4, "video queue")):
            waits = [num(r, "input_wait_us") for r in rows if num(r, "input_stage") == stage]
            if waits:
                print(f"    longest observed {name:12s} wait {max(waits) / 1000:.2f} ms")

    if seconds > 0 and "software_frames" in rows[0]:
        delta = lambda key: num(rows[-1], key) - num(rows[0], key)
        if delta("software_frames") or delta("software_bytes"):
            print("\n  software audio producer (elapsed time, not CPU utilization):")
            print(f"    decoded PCM {delta('software_frames') / 48000:.3f} seconds")
            print(f"    parser/decode/convert work {delta('software_work_us') / 1e6:.3f} seconds")
            print(f"    PCM queue callback {delta('software_queue_us') / 1e6:.3f} seconds")
            print(f"    encoded input {delta('software_bytes') / 1048576:.3f} MiB")

    late = [r for r in rows[1:] if num(r, "loop_us") > 20000]
    print(f"\n  iterations over 20 ms: {len(late)} of {len(rows) - 1}"
          f"  ({100 * len(late) / (len(rows) - 1):.2f}%)")

    shown = [i for i, r in enumerate(rows) if r.get("advanced") == "1"]
    gaps = [(num(rows[b], "at_us") - num(rows[a], "at_us")) / 1000.0
            for a, b in zip(shown, shown[1:])]
    if gaps:
        cadence = sorted(Counter(round(g / 16.68) for g in gaps).items())
        print(f"  picture cadence (vblanks held): {cadence}")
        # First/last advances omit an edge stall, which previously made an
        # under-delivered recording look like perfect 24 fps playback.
        print(f"  cadence between advances {1000 / st.mean(gaps):.2f} fps (excludes edge stalls)")
        delivered = (num(rows[-1], "shown") - num(rows[0], "shown")) if "shown" in rows[0] else sum(num(r, "advanced") for r in rows[1:])
        print(f"  whole-window shown rate {delivered / seconds:.2f} fps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
