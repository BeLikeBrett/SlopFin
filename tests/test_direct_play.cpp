/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "direct_play.hpp"
#include <cassert>
#include <cstdio>

using namespace slopfin::jellyfin;

static PlaybackPlan track(const char *codec, int channels, int rate = 48000, int ordinal = 0)
{
    PlaybackPlan plan;
    plan.audio_codec = codec;
    plan.audio_channels = channels;
    plan.audio_sample_rate = rate;
    plan.audio_ordinal = ordinal;
    return plan;
}

int main()
{
    PlaybackRequest plain;
    PlaybackRequest bitstream;
    bitstream.allow_bitstream = true;
    PlaybackRequest software;
    software.allow_software_audio = true;

    /* The console's own decoders, at 48 kHz. */
    assert(direct_audio_supported(track("aac", 6), plain));
    assert(direct_audio_supported(track("ac3", 6), plain));
    assert(direct_audio_supported(track("mp3", 2), plain));
    assert(!direct_audio_supported(track("aac", 2, 44100), plain));
    assert(!direct_audio_supported(track("ac3", 8), plain));
    /* E-AC-3 and DTS need the TV or the CPU decoder. */
    assert(!direct_audio_supported(track("eac3", 6), plain));
    assert(direct_audio_supported(track("eac3", 6), bitstream));
    assert(direct_audio_supported(track("eac3", 8), software));
    assert(direct_audio_supported(track("dts", 6), bitstream));
    PlaybackPlan dts_ma = track("dts", 8);
    dts_ma.audio_profile = "DTS-HD MA";
    assert(!direct_audio_supported(dts_ma, software));
    assert(!direct_audio_supported(track("truehd", 8), bitstream));
    assert(direct_audio_supported(track("truehd", 8), software));
    assert(!direct_audio_supported(track("flac", 2), bitstream));
    assert(!direct_audio_supported(track("opus", 2), software));
    /* A track in a separate file, a channel limit, or a chosen conversion. */
    assert(!direct_audio_supported(track("aac", 2, 48000, -1), plain));
    PlaybackRequest stereo;
    stereo.max_audio_channels = 2;
    assert(!direct_audio_supported(track("aac", 6), stereo));
    PlaybackRequest convert;
    convert.audio_codec = "aac";
    assert(!direct_audio_supported(track("ac3", 6), convert));
    assert(direct_audio_supported(track("aac", 2), convert));
    /* No audio at all plays. */
    assert(direct_audio_supported(PlaybackPlan{}, plain));
    std::puts("direct play audio checks passed");
    return 0;
}
