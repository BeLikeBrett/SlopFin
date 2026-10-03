# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console helpers use explicit local configuration, never execute it."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("console_config", Path(__file__).resolve().parent.parent / "tools/console_config.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ConsoleConfigTests(unittest.TestCase):
    def test_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".env"
            with self.assertRaisesRegex(ValueError, "Set PS5_HOST"):
                module.console_host(path, {})
            for value in ('192.168.1.20', '"console.local" # comment', "'console.local'", 'console.local # comment'):
                path.write_text("IGNORED=$(touch never-run)\nPS5_HOST=" + value + "\n")
                expected = "192.168.1.20" if value == "192.168.1.20" else "console.local"
                self.assertEqual(module.console_host(path, {}), expected)
                self.assertEqual(module.console_host(path, {"PS5_HOST": "override.local"}), "override.local")
            path.write_text('PS5_HOST="unterminated\n')
            with self.assertRaisesRegex(ValueError, "quoting"):
                module.console_host(path, {})
