# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "build/tests/update_package_tests"
TRANSACTION = ROOT / "build/tests/update_transaction_tests"

class Updates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.files = {"eboot.bin": b"e" * 2048, "sce_module/libc.prx": b"l" * 2048,
                      "sce_sys/param.json": json.dumps({"titleId": "PPSA99001", "contentVersion": "01.000.001"}).encode(),
                      "assets/font.ttf": bytes(range(256)) * 32,
                      "assets/font-regular.ttf": b"r" * 1024,
                      "assets/font-medium.ttf": b"m" * 1024,
                      "assets/font-bold.ttf": b"b" * 1024,
                      "assets/slopfin-update.bin": b"u" * 1024,
                      "assets/slopfin-sandbox.bin": b"s" * 1024}

    def archive(self, changes=None, compression=zipfile.ZIP_DEFLATED):
        files = self.files | (changes or {})
        archive = self.root / "update.zip"
        with zipfile.ZipFile(archive, "w", compression=compression) as out:
            for name, data in files.items():
                out.writestr("PPSA99001/" + name, data)
        return archive

    def stage(self, archive):
        return subprocess.run([PACKAGE, archive, self.root / "staged", "01.000.001"], capture_output=True)

    def test_extract_matches_original_bytes(self):
        for compression in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
            self.assertEqual(self.stage(self.archive(compression=compression)).returncode, 0)
            for name, data in self.files.items():
                self.assertEqual((self.root / "staged" / name).read_bytes(), data)
            manifest = (self.root / "staged/manifest").read_text()
            for name, data in self.files.items():
                self.assertIn(hashlib.sha256(data).hexdigest() + " " + name + "\n", manifest)

    def test_reject_path_escape(self):
        for name in ("../escape", "assets/../../escape", "assets/evil\\file", "config.json", "assets/a\0evil"):
            with self.subTest(name=name):
                # zipfile truncates NUL names, so that case is still an invalid root file.
                archive = self.archive({name: b"bad"})
                if "\0" in name:
                    raw = archive.read_bytes().replace(b"assets/a", b"assets/\0")
                    archive.write_bytes(raw)
                self.assertNotEqual(self.stage(archive).returncode, 0)
        self.assertFalse((self.root / "escape").exists())

    def test_reject_wrong_version_and_crc(self):
        self.assertNotEqual(self.stage(self.archive({"sce_sys/param.json": b'{"titleId":"PPSA99001","contentVersion":"01.000.009"}'})).returncode, 0)
        archive = self.archive(compression=zipfile.ZIP_STORED)
        raw = archive.read_bytes().replace(b"e" * 2048, b"f" * 2048)
        archive.write_bytes(raw)
        self.assertNotEqual(self.stage(archive).returncode, 0)

    def test_reject_symlink(self):
        archive = self.archive()
        with zipfile.ZipFile(archive, "a") as out:
            item = zipfile.ZipInfo("PPSA99001/assets/link")
            item.create_system = 3
            item.external_attr = (0o120777 << 16)
            out.writestr(item, b"/etc/passwd")
        self.assertNotEqual(self.stage(archive).returncode, 0)

    def test_reject_symlinked_storage(self):
        archive = self.archive()
        linked_archive = self.root / "linked.zip"
        linked_archive.symlink_to(archive)
        self.assertNotEqual(self.stage(linked_archive).returncode, 0)
        stage = self.root / "staged"
        stage.mkdir(exist_ok=True)
        outside = self.root / "outside"
        outside.mkdir()
        (stage / "assets").symlink_to(outside)
        self.assertNotEqual(self.stage(archive).returncode, 0)
        self.assertEqual(list(outside.iterdir()), [])

    def prepare_install(self):
        self.assertEqual(self.stage(self.archive()).returncode, 0)
        target = self.root / "installed"
        old = {"eboot.bin": b"old executable", "sce_module/libc.prx": b"old runtime",
               "sce_sys/param.json": b"old metadata"}
        for name, data in old.items():
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if name == "eboot.bin" else 0o640)
        config = self.root / "config.json"
        config.write_bytes(b"unchanged account preferences")
        return target, old, config

    def install(self, target, fail_after=-1):
        return subprocess.run([TRANSACTION, self.root / "staged", target, self.root / "backup", str(fail_after)], capture_output=True)

    def test_install_and_keep_settings(self):
        target, old, config = self.prepare_install()
        self.assertEqual(self.install(target).returncode, 0)
        for name, data in self.files.items():
            self.assertEqual((target / name).read_bytes(), data)
            expected_mode = 0o755 if name == "eboot.bin" or name.startswith("sce_module/") else 0o644
            self.assertEqual((target / name).stat().st_mode & 0o777, expected_mode)
        for name, data in old.items():
            self.assertEqual((self.root / "backup" / name).read_bytes(), data)
        self.assertEqual(config.read_bytes(), b"unchanged account preferences")

    def test_rollback_after_partial_install(self):
        target, old, config = self.prepare_install()
        self.assertEqual(self.install(target, 2).returncode, 4)
        for name, data in old.items():
            self.assertEqual((target / name).read_bytes(), data)
            self.assertEqual((target / name).stat().st_mode & 0o777,
                             0o755 if name == "eboot.bin" else 0o640)
        self.assertFalse((target / "assets/font.ttf").exists())
        self.assertEqual(config.read_bytes(), b"unchanged account preferences")

    def test_tampered_stage_and_target_symlink(self):
        target, old, _ = self.prepare_install()
        (self.root / "staged/eboot.bin").write_bytes(b"tampered")
        self.assertEqual(self.install(target).returncode, 3)
        for name, data in old.items():
            self.assertEqual((target / name).read_bytes(), data)
        self.assertEqual(self.stage(self.archive()).returncode, 0)
        outside = self.root / "outside"
        outside.mkdir()
        (target / "assets").symlink_to(outside)
        self.assertEqual(self.install(target).returncode, 3)
        self.assertEqual(list(outside.iterdir()), [])

if __name__ == "__main__":
    unittest.main()
