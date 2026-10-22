#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add a self-authored PGS square to the existing synthetic video fixture."""
from pathlib import Path
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent

def segment(kind, body):
    return bytes([kind]) + struct.pack('>H', len(body)) + body

def display(clear):
    pcs = struct.pack('>HHBHBBBB', 1920, 1080, 0x10, 0, 0x80, 0, 0, 0 if clear else 1)
    if not clear:
        pcs += struct.pack('>HBBHH', 0, 0, 0, 100, 900)
    pieces = [(0x16, pcs)]
    if not clear:
        pieces += [(0x14, bytes([0, 0, 0, 16, 128, 128, 0, 1, 235, 128, 128, 255])),
                   (0x15, bytes([0, 0, 0, 0xc0, 0, 0, 14, 0, 4, 0, 2,
                                 1, 1, 1, 1, 0, 0, 1, 1, 1, 1]))]
    pieces.append((0x80, b''))
    return pieces

with tempfile.TemporaryDirectory() as directory:
    sup = Path(directory) / 'square.sup'
    content = bytearray()
    for seconds, clear in ((.5, False), (1.5, True), (2.5, False), (3.5, True)):
        for kind, body in display(clear):
            content += b'PG' + struct.pack('>II', int(seconds * 90000), 0) + segment(kind, body)
    sup.write_bytes(content)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i',
                    str(root / 'tests/media/h264_aac_ac3_srt.mkv'), '-i', str(sup),
                    '-map', '0:v:0', '-map', '0:a:0', '-map', '0:s:0', '-map', '1:s:0',
                    '-c', 'copy', '-map_metadata', '-1', '-fflags', '+bitexact',
                    str(root / 'tests/media/h264_aac_srt_pgs.mkv')], check=True)

# Matroska's default ContentCompression is zlib, as used by real PGS tracks.
# Rebuild element sizes and omit indexes whose file offsets would change.
import zlib

def elements(data):
    at = 0
    while at < len(data):
        first = data[at]
        ilen = next(n for n in range(1, 5) if first & (0x80 >> (n - 1)))
        ident = data[at:at+ilen]
        at += ilen
        slen = next(n for n in range(1, 9) if data[at] & (0x80 >> (n - 1)))
        size = int.from_bytes(data[at:at+slen], 'big') & ((1 << (7*slen)) - 1)
        at += slen
        yield ident, data[at:at+size]
        at += size

def element(ident, body):
    n = next(n for n in range(1, 9) if len(body) < (1 << (7*n)) - 1)
    return ident + (len(body) | (1 << (7*n))).to_bytes(n, 'big') + body

def rewrite(data, level):
    out = bytearray()
    for ident, body in elements(data):
        key = ident.hex()
        if key in ('114d9b74', '1c53bb6b'):
            continue
        if key in ('18538067', '1654ae6b', '1f43b675', 'a0'):
            body = rewrite(body, key)
        elif level == '1654ae6b' and key == 'ae' and b'S_HDMV/PGS' in body:
            body += element(bytes.fromhex('6d80'), element(bytes.fromhex('6240'),
                            element(bytes.fromhex('5034'), b'')))
        elif level in ('1f43b675', 'a0') and key in ('a3', 'a1') and body[0] == 0x84:
            body = body[:4] + zlib.compress(body[4:])
        out += element(ident, body)
    return out

media = root / 'tests/media'
(media / 'h264_aac_srt_pgs_zlib.mkv').write_bytes(
    rewrite((media / 'h264_aac_srt_pgs.mkv').read_bytes(), 'root'))
