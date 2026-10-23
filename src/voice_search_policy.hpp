/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_VOICE_SEARCH_POLICY_HPP
#define SLOPFIN_VOICE_SEARCH_POLICY_HPP
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace slopfin::voice_search::detail
{
constexpr int kRate = 16000;
constexpr int kGrain = 256;
constexpr int kMaxSamples = 12 * kRate;
/* Simple energy gating only decides when to stop a short search utterance; the
 * recogniser still applies its own no-speech decision. Muted blocks are zeroed
 * and never contribute to speech detection. */
struct CaptureGate
{
    int samples = 0;
    int speech_samples = 0;
    int quiet_samples = 0;
    float level = 0;
    bool accept(std::span<std::int16_t> block, bool muted) noexcept
    {
        double energy = 0;
        for (auto &sample : block)
        {
            if (muted)
                sample = 0;
            const double value = sample / 32768.0;
            energy += value * value;
        }
        const double rms = block.empty() ? 0 : std::sqrt(energy / block.size());
        level = static_cast<float>(std::min(1.0, rms * 8));
        samples += static_cast<int>(block.size());
        if (!muted && rms > 0.006)
        {
            speech_samples += static_cast<int>(block.size());
            quiet_samples = 0;
        }
        else
            quiet_samples += static_cast<int>(block.size());
        return samples < kMaxSamples &&
               !(speech_samples >= kRate / 5 && quiet_samples >= kRate * 6 / 5);
    }
    bool heard_speech() const noexcept
    {
        return speech_samples >= kRate / 5;
    }
};
inline std::string clean_query(std::string_view source)
{
    std::string out;
    bool space = false;
    for (unsigned char ch : source)
    {
        if (ch <= 32 || ch == 127)
        {
            space = !out.empty();
            continue;
        }
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += static_cast<char>(ch);
        if (out.size() >= 240)
            break;
    }
    /* A partial UTF-8 character at the search-size limit is not a valid query. */
    if (out.size() >= 240)
    {
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0)
            out.pop_back();
    }
    while (!out.empty() &&
           (out.back() == '.' || out.back() == '?' || out.back() == '!' || out.back() == ' '))
        out.pop_back();
    return out;
}
} // namespace slopfin::voice_search::detail
#endif
