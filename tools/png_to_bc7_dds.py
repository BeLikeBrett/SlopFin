#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""SlopFin - encode a PNG as a BC7 DX10 DDS for the PS5 launcher backgrounds.

The console wants 4K BC7_UNORM_SRGB, and the upstream tooling reaches for
Microsoft's Windows-only texconv to produce it. This writes the same thing on
Linux using BC7 mode 6: one subset, RGBA endpoints at 7 bits plus a shared
parity bit, and 4-bit interpolation indices. Mode 6 is the right trade here
because launcher art is broad gradients, which is exactly what a single-subset
mode reproduces well.

Usage: png_to_bc7_dds.py <input.png> <output.dds> [--header-from existing.dds]
"""

import sys
import numpy as np
from PIL import Image

# BC7 4-bit interpolation weights, from the format specification.
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
                   dtype=np.int32)

DDS_HEADER_BYTES = 148  # 128-byte DDS header plus the 20-byte DX10 extension


def build_header(width: int, height: int, template: bytes | None) -> bytes:
    """Reuse a known-good header when one is offered; otherwise synthesise one."""
    if template is not None and len(template) >= DDS_HEADER_BYTES:
        header = bytearray(template[:DDS_HEADER_BYTES])
        header[12:16] = height.to_bytes(4, "little")
        header[16:20] = width.to_bytes(4, "little")
        header[20:24] = (width * height).to_bytes(4, "little")
        return bytes(header)

    header = bytearray(DDS_HEADER_BYTES)
    header[0:4] = b"DDS "
    header[4:8] = (124).to_bytes(4, "little")            # header size
    header[8:12] = (0x000A1007).to_bytes(4, "little")    # caps|height|width|pitch|pixelformat|mipmap
    header[12:16] = height.to_bytes(4, "little")
    header[16:20] = width.to_bytes(4, "little")
    header[20:24] = (width * height).to_bytes(4, "little")  # linear size, 1 byte per pixel
    header[28:32] = (1).to_bytes(4, "little")            # mip count
    header[76:80] = (32).to_bytes(4, "little")           # pixel format size
    header[80:84] = (0x4).to_bytes(4, "little")          # DDPF_FOURCC
    header[84:88] = b"DX10"
    header[108:112] = (0x1000).to_bytes(4, "little")     # DDSCAPS_TEXTURE
    header[128:132] = (98).to_bytes(4, "little")         # DXGI_FORMAT_BC7_UNORM_SRGB
    header[132:136] = (3).to_bytes(4, "little")          # D3D10_RESOURCE_DIMENSION_TEXTURE2D
    header[140:144] = (1).to_bytes(4, "little")          # array size
    return bytes(header)


def to_blocks(image: np.ndarray) -> np.ndarray:
    """Reshape HxWx4 pixels into (blocks, 16, 4) in BC7's row-major block order."""
    height, width, _ = image.shape
    blocks = image.reshape(height // 4, 4, width // 4, 4, 4)
    blocks = blocks.transpose(0, 2, 1, 3, 4)
    return blocks.reshape(-1, 16, 4)


def quantise_endpoint(target: np.ndarray, parity: int) -> np.ndarray:
    """Best 7-bit value for an 8-bit target given the endpoint's parity bit."""
    value = (target.astype(np.int32) - parity + 1) >> 1
    return np.clip(value, 0, 127)


def encode(pixels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Encode (blocks, 16, 4) uint8 pixels into two uint64 halves per block."""
    count = pixels.shape[0]
    low = np.zeros(count, dtype=np.uint64)
    high = np.zeros(count, dtype=np.uint64)

    values = pixels.astype(np.int32)
    lo_target = values.min(axis=1)
    hi_target = values.max(axis=1)

    # Pick each endpoint's parity bit by measuring both reconstructions.
    best_parity = []
    best_seven = []
    for target in (lo_target, hi_target):
        candidates = []
        for parity in (0, 1):
            seven = quantise_endpoint(target, parity)
            rebuilt = (seven << 1) | parity
            error = ((rebuilt - target) ** 2).sum(axis=1)
            candidates.append((error, seven, parity))
        use_one = candidates[1][0] < candidates[0][0]
        best_parity.append(use_one.astype(np.int32))
        best_seven.append(np.where(use_one[:, None], candidates[1][1], candidates[0][1]))

    parity0, parity1 = best_parity
    seven0, seven1 = best_seven
    endpoint0 = (seven0 << 1) | parity0[:, None]
    endpoint1 = (seven1 << 1) | parity1[:, None]

    # Choose, per pixel, the interpolation step closest to the original colour.
    best_error = np.full((count, 16), np.iinfo(np.int32).max, dtype=np.int32)
    indices = np.zeros((count, 16), dtype=np.int32)
    for step, weight in enumerate(WEIGHTS):
        colour = (endpoint0 * (64 - weight) + endpoint1 * weight + 32) >> 6
        error = ((values - colour[:, None, :]) ** 2).sum(axis=2)
        better = error < best_error
        best_error = np.where(better, error, best_error)
        indices = np.where(better, step, indices)

    # The anchor index carries an implicit high bit of zero. When it exceeds 7,
    # swap the endpoints and mirror every index instead.
    flip = indices[:, 0] > 7
    indices = np.where(flip[:, None], 15 - indices, indices)
    seven0, seven1 = (np.where(flip[:, None], seven1, seven0),
                      np.where(flip[:, None], seven0, seven1))
    parity0, parity1 = (np.where(flip, parity1, parity0), np.where(flip, parity0, parity1))

    low |= np.uint64(1) << np.uint64(6)  # mode 6 marker
    for channel in range(4):
        low |= seven0[:, channel].astype(np.uint64) << np.uint64(7 + channel * 14)
        low |= seven1[:, channel].astype(np.uint64) << np.uint64(14 + channel * 14)
    low |= parity0.astype(np.uint64) << np.uint64(63)

    high |= parity1.astype(np.uint64)
    high |= (indices[:, 0].astype(np.uint64) & np.uint64(7)) << np.uint64(1)
    for pixel in range(1, 16):
        high |= indices[:, pixel].astype(np.uint64) << np.uint64(4 + (pixel - 1) * 4)
    return low, high


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    source, destination = sys.argv[1], sys.argv[2]
    template = None
    if "--header-from" in sys.argv:
        with open(sys.argv[sys.argv.index("--header-from") + 1], "rb") as handle:
            template = handle.read(DDS_HEADER_BYTES)

    image = Image.open(source).convert("RGBA")
    width, height = image.size
    if width % 4 or height % 4:
        print(f"dimensions must be multiples of 4, got {width}x{height}", file=sys.stderr)
        return 1

    pixels = np.asarray(image, dtype=np.uint8)
    low, high = encode(to_blocks(pixels))

    payload = np.empty((low.size, 2), dtype="<u8")
    payload[:, 0] = low
    payload[:, 1] = high

    with open(destination, "wb") as handle:
        handle.write(build_header(width, height, template))
        handle.write(payload.tobytes())
    print(f"wrote {destination}: {width}x{height} BC7, {low.size} blocks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
