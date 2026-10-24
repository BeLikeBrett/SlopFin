#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mesa optional-hook linking, independent of a PS5 or a full driver build."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gpu_weak_entrypoints", ROOT / "tools/gpu-weak-entrypoints.py"
)
HOOKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HOOKS)


class GpuLinkTests(unittest.TestCase):
    def test_only_absent_weak_only_mesa_hooks_become_null(self):
        symbols = """library.a[dispatch.o]:
radv_Optional w 0 0
vk_common_Optional w 0 0
radv_Implemented w 0 0
radv_Required w 0 0
malloc w 0 0
sqtt_GetDeviceProcAddr w 0 0
wsi_GetDeviceProcAddr w 0 0
sqtt_Implemented w 0 0
sqtt_Implemented T 0 10
sqtt_Required w 0 0
sqtt_Required U
other_Optional w 0 0
radv_internal_helper w 0 0
radv_Required U
radv_Implemented T 0 10
"""
        self.assertEqual(
            HOOKS.null_entrypoints(symbols),
            ["radv_Optional", "sqtt_GetDeviceProcAddr", "vk_common_Optional", "wsi_GetDeviceProcAddr"],
        )

    def test_link_keeps_implementation_and_rejects_missing_required_function(self):
        cc = shutil.which("clang")
        linker = os.environ.get("GPU_TEST_LLD") or shutil.which("ld.lld")
        nm = shutil.which("llvm-nm")
        ar = shutil.which("llvm-ar")
        readelf = shutil.which("llvm-readelf")
        if not all((cc, linker, nm, ar, readelf)):
            self.skipTest("Clang/LLVM link tools are unavailable")
        with tempfile.TemporaryDirectory(prefix="slopfin-gpu-link-") as directory:
            work = Path(directory)

            def run(*command, check=True):
                return subprocess.run(
                    command, cwd=work, check=check, capture_output=True, text=True
                )

            optional = [prefix + "_Optional" for prefix in HOOKS.MESA_LAYERS]
            (work / "hooks.c").write_text(
                "".join("extern void " + name + "(void) __attribute__((weak));\n"
                        for name in optional) +
                "extern void radv_Implemented(void) __attribute__((weak));\n"
                "void (*hooks[])(void) = {" + ", ".join(optional) + ", radv_Implemented};\n"
                "void _start(void) {}\n"
            )
            (work / "implementation.c").write_text("void radv_Implemented(void) {}\n")
            for source in ("hooks", "implementation"):
                run(cc, "-target", "x86_64-unknown-freebsd", "-fPIC", "-c",
                    source + ".c", "-o", source + ".o")
            run(ar, "rcs", "driver.a", "hooks.o", "implementation.o")
            flags = run(
                "python3", str(ROOT / "tools/gpu-weak-entrypoints.py"),
                "--nm", nm, "--archive", "driver.a",
            ).stdout.splitlines()
            self.assertEqual(flags, ["--defsym=" + name + "=0" for name in sorted(optional)])
            (work / "local.map").write_text("{ local: " + " ".join(name + ";" for name in optional) +
                                           " radv_Implemented; };\n")
            link = (linker, "-pie", "--version-script", "local.map", *flags,
                    "--whole-archive", "driver.a", "--no-whole-archive")
            run(*link, "-o", "good.elf")
            dynamic = run(readelf, "--dyn-syms", "good.elf").stdout
            for name in optional:
                self.assertNotIn(name, dynamic)
            symbols = run(nm, "--format=posix", "good.elf").stdout
            for name in optional:
                self.assertRegex(symbols, name + r" [aA] 0 ")
            self.assertRegex(symbols, r"radv_Implemented [tT] [1-9a-f][0-9a-f]* ")

            (work / "required.c").write_text(
                "extern void radv_MissingRequired(void);\n"
                "void (*required)(void) = radv_MissingRequired;\n"
            )
            run(cc, "-target", "x86_64-unknown-freebsd", "-fPIC", "-c",
                "required.c", "-o", "required.o")
            failed = run(*link, "required.o", "-o", "bad.elf", check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("radv_MissingRequired", failed.stderr)


if __name__ == "__main__":
    unittest.main()
