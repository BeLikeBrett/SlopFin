#!/usr/bin/env python3
"""Generate local codec probes and FFmpeg-decoded reference PCM. No deployment.
Copyright (C) 2026 Brett. SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="build/audio-fixtures")
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    # Standard order FL, FR, FC, LFE, BL, BR; each speaker has an identifiable tone.
    frequencies = [317, 419, 521, 83, 631, 743]
    cases = [
        ("aac_20_44100", "aac", "adts", 44100, 2, "192k"),
        ("aac_20", "aac", "adts", 48000, 2, "192k"),
        ("aac_51", "aac", "adts", 48000, 6, "640k"),
        ("ac3_20", "ac3", "ac3", 48000, 2, "192k"),
        ("ac3_51", "ac3", "ac3", 48000, 6, "640k"),
        ("eac3_20", "eac3", "eac3", 48000, 2, "192k"),
        ("eac3_51", "eac3", "eac3", 48000, 6, "640k"),
        ("dts_51", "dca", "dts", 48000, 6, "1411200"),
        ("truehd_51", "truehd", "truehd", 48000, 6, None),
    ]
    manifest = {"scope": "Synthetic host fixtures, not PS5 decoder validation",
                "ffmpeg": subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0],
                "cases": []}
    for name, codec, container, rate, channels, bitrate in cases:
        layout = "stereo" if channels == 2 else "5.1"
        source = output / f"source_{name}.wav"
        case_frequencies = frequencies[:channels].copy()
        # Keep the DTS encoder LFE fixture below its low-rate analysis Nyquist.
        # 83 Hz aliases in this host encoder; a decoder comparison alone hid it.
        if codec == "dca":
            case_frequencies[3] = 31
        tones = "|".join(f"0.08*sin(2*PI*{hz}*t)" for hz in case_frequencies)
        run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
            f"aevalsrc={tones}:s={rate}:d=3:c={layout}", "-c:a", "pcm_s24le", str(source))
        encoded = output / f"{name}.bin"
        command = ["ffmpeg", "-v", "error", "-y", "-i", str(source), "-c:a", codec]
        if codec in ("dca", "truehd"):
            command += ["-strict", "-2"]
        if bitrate:
            command += ["-b:a", bitrate]
        run(*command, "-f", container, str(encoded))
        reference = output / f"{name}.reference.wav"
        run("ffmpeg", "-v", "error", "-y", "-f", "aac" if container == "adts" else container,
            "-i", str(encoded), "-c:a", "pcm_s16le", str(reference))
        stream = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_entries", "stream=sample_rate,channels,channel_layout",
            "-of", "json", str(reference)], text=True))["streams"][0]
        if int(stream["sample_rate"]) != rate or stream["channels"] != channels:
            raise RuntimeError(f"Unexpected reference format for {name}: {stream}")
        manifest["cases"].append({
            "name": name, "codec": codec, "sample_rate": rate, "channels": channels,
            "source_layout": layout, "reference_layout": stream.get("channel_layout", layout),
            "tone_hz": case_frequencies, "encoded": encoded.name,
            "reference": reference.name, "sha256": hashlib.sha256(encoded.read_bytes()).hexdigest(),
        })
        print(f"generated {name}")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Wrote {output / 'manifest.json'}; nothing sent to the console.")


if __name__ == "__main__":
    main()
