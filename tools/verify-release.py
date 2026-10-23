#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shipped folder ZIP with the updater's actual archive reader."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile


def verify(archive: Path, verifier: Path) -> None:
    # These are also limits in already-installed 1.x updaters. Raising only
    # the new client's limits would leave existing users unable to upgrade.
    if not 1024 <= archive.stat().st_size <= 32 * 1024 * 1024:
        raise ValueError("release ZIP must fit existing updaters' 32 MiB download limit")
    with zipfile.ZipFile(archive) as package:
        entries = package.infolist()
        total = sum(entry.file_size for entry in entries)
        if len(entries) > 256 or total > 128 * 1024 * 1024:
            raise ValueError("release exceeds existing updaters' extraction limits")
        metadata = json.loads(package.read("PPSA99001/sce_sys/param.json"))
    version = metadata["contentVersion"]
    parts = version.split(".")
    if len(parts) != 3 or [len(p) for p in parts] != [2, 3, 3] or not all(p.isdigit() for p in parts):
        raise ValueError("release metadata has an invalid platform version")
    display = f"{int(parts[0])}.{int(parts[1]) * 1000 + int(parts[2]):02d}"
    if archive.name != f"SlopFin-{display}-folder.zip":
        raise ValueError("release filename must match the tag and installed version")
    with tempfile.TemporaryDirectory(prefix="slopfin-release-") as folder:
        subprocess.run([str(verifier.resolve()), str(archive.resolve()), folder, version], check=True)
        manifest = (Path(folder) / "manifest").read_text().splitlines()
        if len(manifest) != sum(not entry.is_dir() for entry in entries):
            raise ValueError("updater manifest does not cover every shipped file")
    print(f"SlopFin {display}: updater accepted {len(manifest)} files; "
          f"ZIP {archive.stat().st_size:,} / 33,554,432 bytes, extracted {total:,} / 134,217,728 bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--verifier", type=Path, default=Path("build/tests/update_package_tests"))
    args = parser.parse_args()
    try:
        verify(args.archive, args.verifier)
    except (ValueError, KeyError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release validation failed: {error}\n")


if __name__ == "__main__":
    main()
