/*
 * SlopFin - audio capability probe.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SLOPFIN_AUDIOCAPS_HPP
#define SLOPFIN_AUDIOCAPS_HPP

namespace audiocaps
{
/*
 * Ask the console which output formats and decoder codecs it will actually
 * accept, and write the answers to /data/slopfin-audio-caps.txt. Everything
 * else about multichannel support is inference; this is measurement.
 */
void probe() noexcept;
} // namespace audiocaps

#endif
