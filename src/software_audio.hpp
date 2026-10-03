/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in software audio decoder feasibility probe. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_SOFTWARE_AUDIO_HPP
#define SLOPFIN_SOFTWARE_AUDIO_HPP
#include <cstdio>
#include <cstddef>
#include <cstdint>
namespace slopfin::software_audio
{
enum class Codec
{
    truehd,
    eac3,
    dts
};
struct Pcm
{
    const std::int16_t *samples;
    std::size_t frames;
    unsigned channels; // interleaved source layout, resampled to 48000 Hz
    std::int64_t pts;  // 90 kHz, -1 when unknown
    // For each AudioOut standard slot (FL FR FC LFE BL BR SL SR),
    // the source channel index, or -1 for silence. Mono duplicates to L/R.
    int map[8];
};
using Sink = bool (*)(void *, const Pcm &) noexcept;
// Single producer owns all calls. The callback may block for output space;
// its owner must interrupt that wait before destroying this decoder.
class Decoder final
{
    struct State;
    State *state_ = nullptr;

  public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    bool open(Codec codec) noexcept;
    bool feed(const std::uint8_t *data, std::size_t bytes, std::int64_t pts, Sink sink,
              void *opaque) noexcept;
    bool finish(Sink sink, void *opaque) noexcept;
    void close() noexcept;
};
bool probe(const char *name, std::FILE *report) noexcept;
} // namespace slopfin::software_audio
#endif
