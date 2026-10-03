#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run HTTP/SSL integration checks against a disposable local TLS server."""
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Stream interruption intentionally disconnects the client.

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/jellyfin/System/Info/Public":
            body = json.dumps({"ServerName": "Test server", "Id": "fixture"}).encode()
            self.send_response(200)
        elif self.path == "/jellyfin/redirect":
            body = b""
            self.send_response(302)
            self.send_header("Location", "/credentials-must-not-arrive")
        else:
            self.server.unexpected.append(self.path)
            body = b"unexpected request"
            self.send_response(404)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/jellyfin/echo":
            self.server.unexpected.append(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="slopfin-tls-test-") as directory:
        certificate = Path(directory) / "certificate.pem"
        key = Path(directory) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(certificate), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.unexpected = []
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        address = f"https://localhost:{server.server_port}/jellyfin"
        environment = dict(os.environ, NO_PROXY="localhost,127.0.0.1", SSL_CERT_FILE=str(certificate))
        try:
            subprocess.run([str(binary), address], env=environment, check=True, timeout=30)
            environment.pop("SSL_CERT_FILE")
            subprocess.run([str(binary), address, "untrusted"], env=environment, check=True, timeout=30)
            assert not server.unexpected, server.unexpected
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    main()
