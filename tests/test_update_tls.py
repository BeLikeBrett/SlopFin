# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check embedded updater trust anchors against published certificate identities."""
import hashlib
from pathlib import Path
import re
import ssl
import subprocess
import tempfile
import unittest


class UpdateTlsTests(unittest.TestCase):
    def test_public_trust_anchors(self):
        source = (Path(__file__).resolve().parents[1] / "assets/ca-certificates.pem").read_text()
        certs = re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', source, re.S)
        expected = {
            "c90f26f0fb1b4018b22227519b5ca2b53e2ca5b3be5cf18efe1bef47380c5383",
            "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6",
            "e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f",
            "e14ffcad5b0025731006caa43a121a22d8e9700f4fb9cf852f02a708aa5d5666",
        }
        self.assertEqual(len(certs), 123)
        actual = {hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert)).hexdigest() for cert in certs}
        self.assertTrue(expected.issubset(actual))
        original = source.split("\n# ISRG transition roots;", 1)[0]
        self.assertEqual(hashlib.sha256(original.encode()).hexdigest(), "a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505")
        with tempfile.TemporaryDirectory() as directory:
            for index, cert in enumerate(certs):
                path = Path(directory) / f"root-{index}.pem"
                path.write_text(cert)
                info = subprocess.run(["openssl", "x509", "-in", str(path), "-noout", "-text"],
                                      check=True, capture_output=True, text=True).stdout
                self.assertIn("CA:TRUE", info)
                subprocess.run(["openssl", "verify", "-check_ss_sig", "-CAfile", str(path), str(path)],
                               check=True, capture_output=True)
