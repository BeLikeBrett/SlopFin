#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect captures with pixel, geometry, contrast and motion comparisons.

Examples:
    tools/look.py zoom home.png --box 900,540,260,380 --out card.png
    tools/look.py scan home.png --row 700 --from 900 --to 960
    tools/look.py plate home.png --box 915,545,240,360 --colour 202020
    tools/look.py diff before.png after.png --out delta.png
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


# --------------------------------------------------------------------- helpers


def load(path: str) -> np.ndarray:
    """An image as H x W x 4 uint8, alpha included."""
    return np.array(Image.open(path).convert("RGBA"))


def save(pixels: np.ndarray, path: str) -> None:
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(pixels, "RGBA").save(path)
    print(f"wrote {path}  {pixels.shape[1]}x{pixels.shape[0]}")


def parse_box(text: str, image: np.ndarray) -> tuple[int, int, int, int]:
    if not text:
        return 0, 0, image.shape[1], image.shape[0]
    parts = [int(p) for p in text.replace(" ", "").split(",")]
    if len(parts) != 4:
        raise SystemExit("--box wants x,y,w,h")
    x, y, w, h = parts
    x = max(0, min(x, image.shape[1] - 1))
    y = max(0, min(y, image.shape[0] - 1))
    w = max(1, min(w, image.shape[1] - x))
    h = max(1, min(h, image.shape[0] - y))
    return x, y, w, h


def parse_colour(text: str) -> tuple[int, int, int]:
    # removeprefix, not lstrip: lstrip("0x") eats the leading zero of 0d0d12.
    text = text.removeprefix("#").removeprefix("0x").removeprefix("0X")
    if len(text) != 6:
        raise SystemExit("--colour wants RRGGBB")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


def font(size: int) -> ImageFont.ImageFont:
    for candidate in (
        "assets/font-medium.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if pathlib.Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def relative_luminance(rgb: np.ndarray) -> np.ndarray:
    """WCAG relative luminance; rgb is (..., 3) in 0..255."""
    channels = rgb.astype(np.float64) / 255.0
    linear = np.where(
        channels <= 0.04045, channels / 12.92, ((channels + 0.055) / 1.055) ** 2.4
    )
    return 0.2126 * linear[..., 0] + 0.7152 * linear[..., 1] + 0.0722 * linear[..., 2]


# ------------------------------------------------------------------- zoom


def cmd_zoom(args: argparse.Namespace) -> int:
    """Crop and magnify with no interpolation.

    Nearest-neighbour on purpose: the question is what a given pixel is, and
    any smoothing here invents an answer. A 12x magnification of a corner is
    how the anti-aliased edge of a rounded rectangle becomes something that can
    be counted rather than squinted at.
    """
    image = load(args.image)
    x, y, w, h = parse_box(args.box, image)
    crop = image[y : y + h, x : x + w]
    factor = args.factor
    if factor <= 0:
        factor = max(1, min(24, 1600 // max(w, h)))
    grown = np.repeat(np.repeat(crop, factor, axis=0), factor, axis=1)

    if args.gridlines and factor >= 6:
        grown[factor - 1 :: factor, :, :3] //= 2
        grown[:, factor - 1 :: factor, :3] //= 2

    print(f"crop {w}x{h} at ({x},{y}) magnified {factor}x")
    save(grown, args.out)
    return 0


# ------------------------------------------------------------------- scan


def cmd_scan(args: argparse.Namespace) -> int:
    """Print the actual pixel values along one row or column.

    This is the instrument that settles an argument. "The poster sits on a grey
    plate" and "the poster covers its box" predict different numbers at the
    same coordinate, and this prints the number.
    """
    image = load(args.image)
    height, width = image.shape[:2]

    if args.row is not None:
        if not 0 <= args.row < height:
            raise SystemExit(f"row {args.row} is outside 0..{height - 1}")
        start = max(0, args.frm if args.frm is not None else 0)
        end = min(width, args.to if args.to is not None else width)
        line = image[args.row, start:end]
        axis, fixed = "x", f"row y={args.row}"
    else:
        column = args.column if args.column is not None else width // 2
        if not 0 <= column < width:
            raise SystemExit(f"column {column} is outside 0..{width - 1}")
        start = max(0, args.frm if args.frm is not None else 0)
        end = min(height, args.to if args.to is not None else height)
        line = image[start:end, column]
        axis, fixed = "y", f"column x={column}"

    print(f"{fixed}, {axis}={start}..{end - 1}")
    if args.runs:
        # Consecutive identical pixels collapse to one line, which turns a
        # 240-pixel sweep into the handful of flat bands that actually matter.
        run_start = start
        previous = tuple(line[0][:3])
        for offset in range(1, len(line)):
            current = tuple(line[offset][:3])
            if current == previous:
                continue
            print(
                f"  {axis} {run_start:4d}..{start + offset - 1:4d} "
                f"({start + offset - run_start:3d} px)  #{previous[0]:02x}{previous[1]:02x}{previous[2]:02x}"
            )
            run_start = start + offset
            previous = current
        print(
            f"  {axis} {run_start:4d}..{end - 1:4d} "
            f"({end - run_start:3d} px)  #{previous[0]:02x}{previous[1]:02x}{previous[2]:02x}"
        )
    else:
        for offset, pixel in enumerate(line):
            r, g, b, a = (int(v) for v in pixel)
            print(f"  {axis}={start + offset:4d}  #{r:02x}{g:02x}{b:02x}  a={a:3d}")
    return 0


# ------------------------------------------------------------------ plate


def cmd_plate(args: argparse.Namespace) -> int:
    """Count pixels inside a box that are the chrome colour rather than art.

    The reported fault is "the poster sits on a grey rectangle". Expressed as a
    measurement that is: within the card's own rectangle, how many pixels are
    within `tolerance` of the surface colour the palette declares? A poster
    that covers its box answers nearly zero. The colour comes from gfx.hpp, not
    from sampling the image, so this cannot agree with itself.
    """
    image = load(args.image)
    x, y, w, h = parse_box(args.box, image)
    crop = image[y : y + h, x : x + w, :3].astype(np.int16)
    target = np.array(parse_colour(args.colour), dtype=np.int16)

    distance = np.abs(crop - target).max(axis=2)
    hit = distance <= args.tolerance
    count = int(hit.sum())
    total = w * h

    print(f"box {w}x{h} at ({x},{y}), hunting #{args.colour} +/-{args.tolerance}")
    print(f"  {count} of {total} pixels ({100.0 * count / total:.2f}%)")

    if count:
        rows = np.where(hit.any(axis=1))[0]
        cols = np.where(hit.any(axis=0))[0]
        print(
            f"  spans rows {y + rows[0]}..{y + rows[-1]}, "
            f"columns {x + cols[0]}..{x + cols[-1]}"
        )
        # Where it sits tells you what it is: a ring around the edge is a plate
        # showing through anti-aliased corners; a solid block is art that never
        # arrived.
        edge = np.zeros_like(hit)
        edge[: args.border, :] = True
        edge[-args.border :, :] = True
        edge[:, : args.border] = True
        edge[:, -args.border :] = True
        on_edge = int((hit & edge).sum())
        print(
            f"  {on_edge} of them within {args.border}px of the border "
            f"({100.0 * on_edge / max(1, count):.1f}% of the hits)"
        )

    if args.out:
        marked = image.copy()
        region = marked[y : y + h, x : x + w]
        region[hit] = [255, 0, 128, 255]
        save(marked, args.out)
    return 0 if count <= args.allow else 1


# ---------------------------------------------------------------- geometry


def cmd_geometry(args: argparse.Namespace) -> int:
    """Find the real edges of drawn content inside a box.

    Given the rectangle the layout constants say a card occupies, report where
    non-background pixels actually begin and end. A card whose art is inset by
    three pixels is a card with a three-pixel frame of something else, and this
    says so without anyone having to see it.
    """
    image = load(args.image)
    x, y, w, h = parse_box(args.box, image)
    crop = image[y : y + h, x : x + w, :3].astype(np.int16)
    background = np.array(parse_colour(args.background), dtype=np.int16)

    differs = np.abs(crop - background).max(axis=2) > args.tolerance
    if not differs.any():
        print("nothing but background inside that box")
        return 1

    rows = np.where(differs.any(axis=1))[0]
    cols = np.where(differs.any(axis=0))[0]
    print(f"box {w}x{h} at ({x},{y}); background #{args.background}")
    print(f"  content rows    {y + rows[0]}..{y + rows[-1]}  ({rows[-1] - rows[0] + 1} px tall)")
    print(f"  content columns {x + cols[0]}..{x + cols[-1]}  ({cols[-1] - cols[0] + 1} px wide)")
    print(
        f"  insets: left {cols[0]}, right {w - 1 - cols[-1]}, "
        f"top {rows[0]}, bottom {h - 1 - rows[-1]}"
    )
    return 0


# ------------------------------------------------------------------ sheet


def cmd_sheet(args: argparse.Namespace) -> int:
    """Tile several captures into one labelled sheet.

    Comparing four states by opening four files compares them against memory.
    Putting them side by side compares them against each other.
    """
    images = [Image.open(path).convert("RGBA") for path in args.images]
    if not images:
        raise SystemExit("no images")
    cell_w = args.width
    cell_h = max(1, round(cell_w * images[0].height / images[0].width))
    columns = max(1, args.cols)
    rows = (len(images) + columns - 1) // columns
    label_h = 30 if args.labels else 0
    gap = 12

    sheet = Image.new(
        "RGBA",
        (columns * cell_w + (columns + 1) * gap, rows * (cell_h + label_h) + (rows + 1) * gap),
        (18, 18, 22, 255),
    )
    draw = ImageDraw.Draw(sheet)
    glyphs = font(18)

    for index, image in enumerate(images):
        column = index % columns
        row = index // columns
        left = gap + column * (cell_w + gap)
        top = gap + row * (cell_h + label_h + gap)
        sheet.paste(image.resize((cell_w, cell_h), Image.LANCZOS), (left, top))
        if args.labels:
            name = pathlib.Path(args.images[index]).name
            draw.text((left + 2, top + cell_h + 6), name, font=glyphs, fill=(210, 210, 220, 255))

    save(np.array(sheet), args.out)
    return 0


# ------------------------------------------------------------------- diff


def cmd_diff(args: argparse.Namespace) -> int:
    """Amplified A/B difference plus a changed-pixel count.

    A visual change that was supposed to touch only the cards and in fact moved
    the whole page shows up here immediately, and nowhere else.
    """
    first = load(args.first)
    second = load(args.second)
    if first.shape != second.shape:
        raise SystemExit(f"different sizes: {first.shape} vs {second.shape}")

    delta = np.abs(first[..., :3].astype(np.int16) - second[..., :3].astype(np.int16))
    worst = delta.max(axis=2)
    changed = int((worst > args.threshold).sum())
    total = worst.size
    print(f"{changed} of {total} pixels differ by more than {args.threshold} "
          f"({100.0 * changed / total:.3f}%)")
    print(f"  largest single-channel difference: {int(worst.max())}")

    if changed:
        rows = np.where((worst > args.threshold).any(axis=1))[0]
        cols = np.where((worst > args.threshold).any(axis=0))[0]
        print(f"  confined to rows {rows[0]}..{rows[-1]}, columns {cols[0]}..{cols[-1]}")

    if args.out:
        shown = np.clip(delta.astype(np.int32) * args.amplify, 0, 255).astype(np.uint8)
        out = np.dstack([shown, np.full(shown.shape[:2], 255, dtype=np.uint8)])
        save(out, args.out)
    return 0


# ------------------------------------------------------------------- grid


def cmd_grid(args: argparse.Namespace) -> int:
    """Rule a capture with coordinates, so a layout note can name numbers."""
    image = Image.open(args.image).convert("RGBA")
    draw = ImageDraw.Draw(image, "RGBA")
    glyphs = font(14)
    step = args.step

    for x in range(0, image.width, step):
        heavy = x % (step * 4) == 0
        draw.line([(x, 0), (x, image.height)], fill=(255, 80, 160, 110 if heavy else 45))
        if heavy:
            draw.text((x + 3, 3), str(x), font=glyphs, fill=(255, 120, 180, 230))
    for y in range(0, image.height, step):
        heavy = y % (step * 4) == 0
        draw.line([(0, y), (image.width, y)], fill=(255, 80, 160, 110 if heavy else 45))
        if heavy:
            draw.text((3, y + 3), str(y), font=glyphs, fill=(255, 120, 180, 230))

    # The console's title-safe inset, which is the one boundary that is not
    # arbitrary: kSafeX and kSafeY in app.cpp.
    draw.rectangle([96, 54, image.width - 96, image.height - 54], outline=(80, 220, 120, 160))
    save(np.array(image), args.out)
    return 0


# --------------------------------------------------------------- contrast


def cmd_contrast(args: argparse.Namespace) -> int:
    """Text legibility inside a box, as a WCAG ratio.

    Takes the darkest and lightest tenth of the region as the background and
    the ink, which is what a run of anti-aliased glyphs on a scrim actually
    looks like to a sampler.
    """
    image = load(args.image)
    x, y, w, h = parse_box(args.box, image)
    crop = image[y : y + h, x : x + w, :3]
    luminance = relative_luminance(crop)

    flat = np.sort(luminance.ravel())
    dark = float(flat[: max(1, len(flat) // 10)].mean())
    light = float(flat[-max(1, len(flat) // 10) :].mean())
    ratio = (light + 0.05) / (dark + 0.05)

    print(f"box {w}x{h} at ({x},{y})")
    print(f"  darkest tenth  luminance {dark:.4f}")
    print(f"  lightest tenth luminance {light:.4f}")
    print(f"  contrast ratio {ratio:.2f}:1", end="  ")
    if ratio >= 7.0:
        print("(AAA for body text)")
    elif ratio >= 4.5:
        print("(AA for body text, AAA for large)")
    elif ratio >= 3.0:
        print("(AA for large text only)")
    else:
        print("(FAILS -- not legible enough at any size)")
    return 0 if ratio >= args.minimum else 1


# ---------------------------------------------------------------- palette


def cmd_palette(args: argparse.Namespace) -> int:
    """The colours a region is actually made of, most common first."""
    image = load(args.image)
    x, y, w, h = parse_box(args.box, image)
    crop = image[y : y + h, x : x + w, :3]
    quantised = (crop // args.bucket) * args.bucket
    flat = quantised.reshape(-1, 3)
    colours, counts = np.unique(flat, axis=0, return_counts=True)
    order = np.argsort(-counts)

    print(f"box {w}x{h} at ({x},{y}), quantised to {args.bucket}")
    for index in order[: args.top]:
        r, g, b = (int(v) for v in colours[index])
        share = 100.0 * counts[index] / len(flat)
        print(f"  #{r:02x}{g:02x}{b:02x}  {share:6.2f}%  ({int(counts[index])} px)")
    return 0


# ------------------------------------------------------------------- main


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)

    zoom = sub.add_parser("zoom", help="crop and magnify without interpolation")
    zoom.add_argument("image")
    zoom.add_argument("--box", default="", help="x,y,w,h")
    zoom.add_argument("--factor", type=int, default=0, help="0 picks one that fits")
    zoom.add_argument("--gridlines", action="store_true", help="one line per source pixel")
    zoom.add_argument("--out", default="zoom.png")
    zoom.set_defaults(func=cmd_zoom)

    scan = sub.add_parser("scan", help="print pixel values along a row or column")
    scan.add_argument("image")
    scan.add_argument("--row", type=int)
    scan.add_argument("--column", type=int)
    scan.add_argument("--from", dest="frm", type=int)
    scan.add_argument("--to", type=int)
    scan.add_argument("--runs", action="store_true", help="collapse identical neighbours")
    scan.set_defaults(func=cmd_scan)

    plate = sub.add_parser("plate", help="count chrome-coloured pixels inside a box")
    plate.add_argument("image")
    plate.add_argument("--box", default="")
    plate.add_argument("--colour", default="202020", help="palette::surface by default")
    plate.add_argument("--tolerance", type=int, default=6)
    plate.add_argument("--border", type=int, default=4)
    plate.add_argument("--allow", type=int, default=0, help="exit 0 when at or below this")
    plate.add_argument("--out", default="")
    plate.set_defaults(func=cmd_plate)

    geometry = sub.add_parser("geometry", help="where content really starts and ends")
    geometry.add_argument("image")
    geometry.add_argument("--box", default="")
    geometry.add_argument("--background", default="0d0d12")
    geometry.add_argument("--tolerance", type=int, default=10)
    geometry.set_defaults(func=cmd_geometry)

    sheet = sub.add_parser("sheet", help="tile captures into one labelled sheet")
    sheet.add_argument("out")
    sheet.add_argument("images", nargs="+")
    sheet.add_argument("--cols", type=int, default=2)
    sheet.add_argument("--width", type=int, default=640)
    sheet.add_argument("--labels", action="store_true", default=True)
    sheet.set_defaults(func=cmd_sheet)

    diff = sub.add_parser("diff", help="amplified A/B difference")
    diff.add_argument("first")
    diff.add_argument("second")
    diff.add_argument("--out", default="")
    diff.add_argument("--amplify", type=int, default=6)
    diff.add_argument("--threshold", type=int, default=2)
    diff.set_defaults(func=cmd_diff)

    grid = sub.add_parser("grid", help="overlay coordinates and the title-safe box")
    grid.add_argument("image")
    grid.add_argument("--step", type=int, default=40)
    grid.add_argument("--out", default="grid.png")
    grid.set_defaults(func=cmd_grid)

    contrast = sub.add_parser("contrast", help="WCAG contrast inside a box")
    contrast.add_argument("image")
    contrast.add_argument("--box", default="")
    contrast.add_argument("--minimum", type=float, default=4.5)
    contrast.set_defaults(func=cmd_contrast)

    palette = sub.add_parser("palette", help="most common colours in a box")
    palette.add_argument("image")
    palette.add_argument("--box", default="")
    palette.add_argument("--bucket", type=int, default=8)
    palette.add_argument("--top", type=int, default=12)
    palette.set_defaults(func=cmd_palette)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
