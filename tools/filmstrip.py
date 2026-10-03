#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Look at motion, which a screenshot cannot show.

Captures consecutive frames of one animation and lays them out as a strip, then
measures the thing the eye is bad at: whether the movement accelerates, whether
it overshoots, and whether it actually stops. "It feels flat" and "it feels
springy" are the same picture in any single frame and different curves here.

The interesting region is usually a small one -- a card taking the focus, a
list settling -- so a box is cropped from every frame rather than shrinking all
of them into illegibility.

    tools/filmstrip.py --setup "wait 300" --action "right" --frames 14 \\
        --box 330,520,700,430 --out focus.png

Both scripts are host_main.cpp's step language. `--setup` runs first and is not
captured; `--action` is performed, and the frames after it are.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def font(size: int) -> ImageFont.ImageFont:
    for candidate in (
        "assets/font-medium.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if pathlib.Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def capture(setup: str, action: str, frames: int, gap: int, out_dir: pathlib.Path) -> list[pathlib.Path]:
    """One capture per frame, so every frame is a real presented frame."""
    steps = [setup] if setup else []
    if action:
        steps.append(action)
    for index in range(frames):
        # No wait before the first shot: the frame immediately after the action
        # is the one that shows whether the movement starts from rest.
        if index > 0 and gap > 0:
            steps.append(f"wait {gap}")
        steps.append(f"shot f{index:03d}.png")

    script = "; ".join(steps)
    binary = pathlib.Path("build/host/slopfin")
    if not binary.exists():
        subprocess.run(["make", "host"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(
        [
            str(binary),
            "--headless",
            "--exit-after-script",
            "--out",
            str(out_dir),
            "--script",
            script,
        ],
        check=True,
    )
    return sorted(out_dir.glob("f*.png"))


def centroid(pixels: np.ndarray) -> tuple[float, float]:
    """Brightness-weighted centre of a crop: where the bright thing is."""
    grey = pixels[..., :3].astype(np.float64).mean(axis=2)
    total = grey.sum()
    if total <= 0:
        return 0.0, 0.0
    ys, xs = np.mgrid[0 : grey.shape[0], 0 : grey.shape[1]]
    return float((xs * grey).sum() / total), float((ys * grey).sum() / total)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--setup", default="wait 300", help="run before capturing")
    parser.add_argument("--action", default="", help="the press whose result is captured")
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--gap", type=int, default=1, help="frames between captures")
    parser.add_argument("--box", default="", help="x,y,w,h to crop from each frame")
    parser.add_argument("--scale", type=float, default=0.5)
    parser.add_argument("--columns", type=int, default=6)
    parser.add_argument("--out", default="filmstrip.png")
    parser.add_argument("--keep", default="", help="keep the raw frames here")
    args = parser.parse_args(argv)

    work = pathlib.Path(args.keep) if args.keep else pathlib.Path(tempfile.mkdtemp())
    work.mkdir(parents=True, exist_ok=True)
    try:
        shots = capture(args.setup, args.action, args.frames, args.gap, work)
        if not shots:
            print("no frames captured", file=sys.stderr)
            return 1

        box = None
        if args.box:
            box = tuple(int(v) for v in args.box.replace(" ", "").split(","))
            if len(box) != 4:
                raise SystemExit("--box wants x,y,w,h")

        crops: list[Image.Image] = []
        centres: list[tuple[float, float]] = []
        for path in shots:
            image = Image.open(path).convert("RGBA")
            if box:
                image = image.crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
            centres.append(centroid(np.array(image)))
            crops.append(
                image.resize(
                    (max(1, int(image.width * args.scale)), max(1, int(image.height * args.scale))),
                    Image.LANCZOS,
                )
            )

        # The measurement. Frame-to-frame movement of the bright centre is the
        # velocity; a flat interface starts fast and decays, a sprung one
        # starts slow, peaks, and can pass the target and come back.
        print(f"{len(crops)} frames, {args.gap} frame(s) apart")
        print("  frame   x        y        dx      dy")
        peak = 0.0
        peak_at = 0
        for index, (x, y) in enumerate(centres):
            dx = x - centres[index - 1][0] if index else 0.0
            dy = y - centres[index - 1][1] if index else 0.0
            speed = abs(dx) + abs(dy)
            if speed > peak:
                peak, peak_at = speed, index
            print(f"  {index:5d}  {x:7.2f}  {y:7.2f}  {dx:6.2f}  {dy:6.2f}")
        if peak_at == 1:
            print("  fastest on the first frame: this is a decay, not a spring")
        elif peak_at > 1:
            print(f"  fastest at frame {peak_at}: it accelerates, which is what a spring does")
        settled = all(
            abs(centres[i][0] - centres[-1][0]) < 0.15 and abs(centres[i][1] - centres[-1][1]) < 0.15
            for i in range(max(0, len(centres) - 3), len(centres))
        )
        print(f"  settled by the end: {'yes' if settled else 'NO - still moving'}")

        columns = max(1, args.columns)
        rows = (len(crops) + columns - 1) // columns
        cell_w, cell_h = crops[0].width, crops[0].height
        label = 22
        gap = 8
        sheet = Image.new(
            "RGBA",
            (columns * cell_w + (columns + 1) * gap, rows * (cell_h + label) + (rows + 1) * gap),
            (16, 16, 20, 255),
        )
        draw = ImageDraw.Draw(sheet)
        glyphs = font(15)
        for index, crop in enumerate(crops):
            left = gap + (index % columns) * (cell_w + gap)
            top = gap + (index // columns) * (cell_h + label + gap)
            sheet.paste(crop, (left, top))
            draw.text(
                (left + 2, top + cell_h + 3),
                f"+{index * max(1, args.gap)}",
                font=glyphs,
                fill=(200, 200, 215, 255),
            )
        pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.out)
        print(f"wrote {args.out}  {sheet.width}x{sheet.height}")
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
