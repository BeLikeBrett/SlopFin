/*
 * SlopFin - HDMI audio bitstream experiment (development only).
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SLOPFIN_BITSTREAM_PROBE_HPP
#define SLOPFIN_BITSTREAM_PROBE_HPP

namespace bitstream_probe
{
/*
 * Triggered by /data/slopfin-bitstream-test. Plays the IEC 61937 stream in
 * /data/slopfin-bitstream-<file>.spdif through libSceAudioOut's "Ex" AV-playback path
 * on a background thread and logs every return code to
 * /data/slopfin-bitstream.txt. See tools/bitstream/README.md.
 */
void start(const char *arguments) noexcept;
} // namespace bitstream_probe

#endif
