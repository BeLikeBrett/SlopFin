#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Source from a console helper to resolve its configured address.
console_tools=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
host=$(python3 "$console_tools/console_config.py") || return 2
export PS5_HOST="$host"
