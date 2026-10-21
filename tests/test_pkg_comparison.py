#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "pkg_compare", ROOT / "tools/native-pkg/compare_libprospero.py"
)
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)


class PackageComparisonTests(unittest.TestCase):
    def test_self_normalization_rejects_a_code_change(self):
        source = bytearray(128)
        source[:4] = b"O\x15=\x1d"
        struct.pack_into("<Q", source, 16, 104)
        source[40:48] = b"app-code"
        source[96:] = bytes(range(32))
        packed = source.copy()
        packed[:4] = b"T\x14\xf5\xee"
        packed[96:96] = b"\0" * 8
        result = COMPARE.normalized_self(source, packed)
        self.assertEqual(result, {"magic_rewritten": True, "trailer_padding": 8})
        packed[45] ^= 1
        with self.assertRaisesRegex(ValueError, "beyond magic"):
            COMPARE.normalized_self(source, packed)

    def test_missing_or_changed_asset_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, extracted = root / "source", root / "extracted"
            source.mkdir()
            extracted.mkdir()
            (source / "LICENSE").write_bytes(b"original")
            with self.assertRaisesRegex(ValueError, "missing source"):
                COMPARE.audit(root / "unused.pkg", source, extracted)
            (extracted / "LICENSE").write_bytes(b"damaged")
            with self.assertRaisesRegex(ValueError, "Payload mismatch"):
                COMPARE.audit(root / "unused.pkg", source, extracted)

    def test_unexpected_rights_module_and_symlinks_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, extracted = root / "source", root / "extracted"
            source.mkdir()
            extracted.mkdir()
            extra = extracted / "right.sprx"
            extra.write_bytes(b"unwanted")
            with self.assertRaisesRegex(ValueError, "unexpected generated"):
                COMPARE.audit(root / "unused.pkg", source, extracted)
            extra.unlink()
            extra.symlink_to(root / "missing")
            with self.assertRaisesRegex(ValueError, "symlinks"):
                COMPARE.audit(root / "unused.pkg", source, extracted)


if __name__ == "__main__":
    unittest.main()
