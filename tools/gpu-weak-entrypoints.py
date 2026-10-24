#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve Mesa's absent optional entrypoints to NULL before PS5 conversion.

LLD 18 leaves default-visibility undefined weak hooks as dynamic imports in
a PIE. Newer LLD resolves them locally. Mesa deliberately uses NULL hooks to
select its common implementation; these are not PS5 system-module imports.
Only unimplemented, weak-only Mesa entrypoints qualify. Defined functions,
strong references and unrelated weak symbols keep their normal link rules.
"""

import argparse
import re
import subprocess


# Generated Mesa dispatch tables include these driver, WSI and tracing layers.
# Keep unrelated weak runtime symbols under the normal linker/import rules.
MESA_LAYERS = (
    "radv", "vk_common", "wsi", "annotate", "ctx_roll", "rmv", "rra",
    "sqtt", "threaded", "utrace", "vk_cmd_enqueue", "vk_cmd_enqueue_unless_primary",
)
MESA_ENTRYPOINT = re.compile(r"(?:" + "|".join(MESA_LAYERS) + r")_[A-Z][A-Za-z0-9_]*")


def null_entrypoints(symbols):
    defined, weak, strong = set(), set(), set()
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) < 2 or len(parts[1]) != 1:
            continue  # archive member headings
        name, kind = parts[:2]
        if kind in ("w", "v"):
            weak.add(name)
        elif kind == "U":
            strong.add(name)
        else:
            defined.add(name)
    return sorted(
        name for name in weak - defined - strong
        if MESA_ENTRYPOINT.fullmatch(name)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--archive", required=True)
    args = parser.parse_args()
    result = subprocess.run(
        [args.nm, "--format=posix", args.archive],
        check=True, capture_output=True, text=True,
    )
    for name in null_entrypoints(result.stdout):
        print(f"--defsym={name}=0")


if __name__ == "__main__":
    main()
