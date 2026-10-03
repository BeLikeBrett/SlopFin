#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the console address without executing the local .env file."""
import os
from pathlib import Path


def console_host(env_file=None, environ=None):
    environ = os.environ if environ is None else environ
    configured = environ.get("PS5_HOST", "").strip()
    if configured:
        return configured
    path = Path(env_file) if env_file is not None else Path(__file__).resolve().parent.parent / ".env"
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.strip().partition("=")
            if separator and key.strip() == "PS5_HOST":
                value = value.strip()
                if value.startswith(("'", '"')):
                    quote = value[0]
                    end = value.find(quote, 1)
                    if end < 0 or (value[end + 1:].strip() and not value[end + 1:].lstrip().startswith("#")):
                        raise ValueError("Invalid PS5_HOST quoting in .env")
                    value = value[1:end]
                else:
                    value = value.split("#", 1)[0].strip()
                if value:
                    return value
    raise ValueError("Set PS5_HOST in the environment or the project's .env file (see .env.example)")


if __name__ == "__main__":
    try:
        print(console_host())
    except ValueError as error:
        raise SystemExit(str(error)) from error
