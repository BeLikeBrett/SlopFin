#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit an extracted LibProspero SlopFin comparison against its staged input.

This checks payload preservation and the four-entry PlayGo digest. It does not
replace full package verification or establish console launch compatibility.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def normalized_self(original, actual):
    if len(original) < 32 or original[:4] not in (b"O\x15=\x1d", b"T\x14\xf5\xee"):
        raise ValueError("Changed executable is not an existing SlopFin SELF")
    padding = len(actual) - len(original)
    marker = struct.unpack_from("<Q", original, 16)[0]
    if not 0 <= padding < 16 or not 32 <= marker - padding <= len(original):
        raise ValueError("Unexpected executable size or trailer location")
    expected = bytearray(original)
    expected[:4] = b"T\x14\xf5\xee"
    expected[marker - padding : marker - padding] = b"\0" * padding
    if expected != actual:
        raise ValueError("Executable differs beyond magic and trailer padding")
    return {"magic_rewritten": original[:4] != actual[:4], "trailer_padding": padding}


def normalized_param(original, actual):
    expected = json.loads(original)
    expected.setdefault("addcont", {})["serviceIdForSharing"] = [" " * 19] * 7
    expected["pubtools"]["creationDate"] = "1970-01-01 00:00:00"
    expected["pubtools"]["toolVersion"] = "2.79"
    expected["versionFileUri"] = " " * 255
    if expected != json.loads(actual):
        raise ValueError("Metadata differs beyond expected publisher normalization")


def playgo_digest(package):
    data = package.read_bytes()
    if data[:4] != b"\x7fFIH":
        raise ValueError("Missing finalized-image header")
    cnt = struct.unpack_from("<Q", data, 0x58)[0]
    body = data[cnt:]
    if body[:4] != b"\x7fCNT":
        raise ValueError("Missing CNT header")
    count = struct.unpack_from(">I", body, 0x10)[0]
    table = struct.unpack_from(">I", body, 0x18)[0]
    if count > 4096 or table + count * 32 > len(body):
        raise ValueError("CNT entry table is out of bounds")
    entries = {}
    for i in range(count):
        ident, _, flags, _, offset, length = struct.unpack_from(">IIIIII", body, table + i * 32)
        if offset + length > len(body) or ident in entries:
            raise ValueError("CNT entry is out of bounds or duplicated")
        if ident in (0x80, 0x1001, 0x2010, 0x2011, 0x3000) and flags & 0x80000000:
            raise ValueError("PlayGo audit requires plaintext entries")
        entries[ident] = body[offset : offset + length]
    sha3 = lambda value: hashlib.sha3_256(value).digest()
    ids = (0x1001, 0x2010, 0x2011, 0x3000)
    expected = sha3(b"".join(sha3(entries[i]) for i in ids))
    actual = entries[0x80][0x20 + 6 * 32 : 0x20 + 7 * 32]
    if expected != actual:
        raise ValueError("Four-entry PlayGo general digest mismatch")
    return {"entry_ids": [hex(i) for i in ids], "sha3_256": actual.hex()}


def audit(package, source, extracted):
    allowed_extras = {
        "sce_sys/icon0.dds", "sce_sys/keystone", "sce_sys/pfs-version.dat",
        "sce_sys/pic0.png", "sce_sys/pic1.png", "sce_sys/playgo-chunk.dat",
        "sce_sys/playgo-ficm.dat", "sce_sys/playgo-hash-table.dat", "sce_sys/playgo-scenario.json",
    }
    originals = {p.relative_to(source).as_posix(): p for p in source.rglob("*") if p.is_file()}
    unpacked = {p.relative_to(extracted).as_posix(): p for p in extracted.rglob("*") if p.is_file()}
    if any(p.is_symlink() for tree in (source, extracted) for p in tree.rglob("*")):
        raise ValueError("Comparison trees must not contain symlinks")
    if originals.keys() - unpacked.keys():
        raise ValueError("Package is missing source files")
    extras = unpacked.keys() - originals.keys()
    if extras - allowed_extras or any(p.name == "right.sprx" for p in unpacked.values()):
        raise ValueError("Package contains unexpected generated files or right.sprx")
    identical = []
    normalized = {}
    for name, path in sorted(originals.items()):
        before, after = path.read_bytes(), unpacked[name].read_bytes()
        if before == after:
            identical.append(name)
        elif name == "sce_sys/param.json":
            normalized_param(before, after)
            normalized[name] = "publisher metadata"
        elif name == "eboot.bin" or name.startswith("sce_module/"):
            normalized[name] = normalized_self(before, after)
        else:
            raise ValueError("Payload mismatch: " + name)
    return {
        "package": package.name, "sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
        "identical_files": identical, "normalized_files": normalized,
        "generated_files": sorted(extras), "playgo_general_digest": playgo_digest(package),
        "source_sha256": {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in sorted(originals.items())},
        "console_launch_verified": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("extracted", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.package, args.source, args.extracted), indent=2))
