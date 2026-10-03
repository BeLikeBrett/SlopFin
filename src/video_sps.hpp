/*
 * SlopFin - what a video stream's sequence parameter set asks of the decoder.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Parse dimensions and reference-picture requirements from the first SPS;
 * server metadata may underreport them and cause decoder error 0x811d0303.
 * Field order follows H.264 7.3.2.1 / E.1.1 and H.265 7.3.2.2 / 7.3.3.
 * Tests compare results with FFmpeg trace_headers.
 */

#ifndef SLOPFIN_VIDEO_SPS_HPP
#define SLOPFIN_VIDEO_SPS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace slopfin::video
{

struct StreamShape
{
    bool valid = false;
    int width = 0; /* coded luma size, before cropping */
    int height = 0;
    int dpb = 0; /* pictures the decoded-picture buffer must hold */
    int bit_depth = 8;
    int level = 0; /* level_idc as coded */
};

namespace detail
{
class Bits
{
  public:
    Bits(const std::uint8_t *data, std::size_t bytes) : data_(data), bits_(bytes * 8)
    {
    }
    [[nodiscard]] bool overrun() const noexcept
    {
        return at_ > bits_;
    }
    std::uint32_t u(unsigned count) noexcept
    {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++at_)
            value = (value << 1) | (at_ < bits_ ? (data_[at_ >> 3] >> (7 - at_ % 8)) & 1u : 0u);
        return value;
    }
    void skip(std::size_t count) noexcept
    {
        at_ += count;
    }
    std::uint32_t ue() noexcept
    {
        unsigned zeros = 0;
        while (u(1) == 0 && zeros < 32 && !overrun())
            ++zeros;
        if (zeros >= 32)
        {
            at_ = bits_ + 1;
            return 0;
        }
        return ((1u << zeros) - 1) + u(zeros);
    }
    std::int32_t se() noexcept
    {
        const std::uint32_t k = ue();
        return (k & 1) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
    }

  private:
    const std::uint8_t *data_;
    std::size_t bits_;
    std::size_t at_ = 0;
};

/* The NAL unit's payload with emulation-prevention bytes removed. */
inline std::vector<std::uint8_t> rbsp(const std::uint8_t *data, std::size_t bytes)
{
    std::vector<std::uint8_t> out;
    out.reserve(bytes);
    int zeros = 0;
    for (std::size_t i = 0; i < bytes; ++i)
    {
        if (zeros >= 2 && data[i] == 3)
        {
            zeros = 0;
            continue;
        }
        zeros = data[i] == 0 ? zeros + 1 : 0;
        out.push_back(data[i]);
    }
    return out;
}

/* Calls visit(nal, bytes) for each Annex-B NAL unit; stops when it returns true. */
template <typename Visit> void each_nal(const std::uint8_t *data, std::size_t bytes, Visit visit)
{
    std::size_t start = 0;
    bool open = false;
    for (std::size_t i = 0; i + 3 <= bytes; ++i)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            if (open)
            {
                std::size_t end = i;
                while (end > start && data[end - 1] == 0)
                    --end;
                if (visit(data + start, end - start))
                    return;
            }
            start = i + 3;
            open = true;
            i += 2;
        }
    }
    if (open && start < bytes)
        (void)visit(data + start, bytes - start);
}

inline int h264_max_dpb_mbs(int level, bool constraint3) noexcept
{
    if (level == 11 && constraint3)
        return 396; /* level 1b */
    switch (level)
    {
    case 9:
    case 10:
        return 396;
    case 11:
        return 900;
    case 12:
    case 13:
    case 20:
        return 2376;
    case 21:
        return 4752;
    case 22:
    case 30:
        return 8100;
    case 31:
        return 18000;
    case 32:
        return 20480;
    case 40:
    case 41:
        return 32768;
    case 42:
        return 34816;
    case 50:
        return 110400;
    case 51:
    case 52:
        return 184320;
    default:
        return level > 52 ? 696320 : 184320;
    }
}

inline void h264_hrd(Bits &b) noexcept
{
    const std::uint32_t count = b.ue() + 1;
    b.u(4);
    b.u(4);
    for (std::uint32_t i = 0; i < count && i < 32 && !b.overrun(); ++i)
    {
        b.ue();
        b.ue();
        b.u(1);
    }
    b.u(20);
}

inline StreamShape h264_sps(const std::vector<std::uint8_t> &sps) noexcept
{
    StreamShape shape;
    if (sps.size() < 4)
        return shape;
    Bits b(sps.data(), sps.size());
    const std::uint32_t profile = b.u(8);
    const std::uint32_t constraints = b.u(8);
    const int level = static_cast<int>(b.u(8));
    b.ue(); /* seq_parameter_set_id */
    std::uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135)
    {
        chroma = b.ue();
        if (chroma == 3)
            b.u(1);
        shape.bit_depth = static_cast<int>(b.ue()) + 8;
        b.ue();
        b.u(1);
        if (b.u(1)) /* seq_scaling_matrix_present_flag */
        {
            for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i)
            {
                if (!b.u(1))
                    continue;
                const int size = i < 6 ? 16 : 64;
                int last = 8, next = 8;
                for (int j = 0; j < size && !b.overrun(); ++j)
                {
                    if (next != 0)
                        next = (last + b.se() + 256) % 256;
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    b.ue(); /* log2_max_frame_num_minus4 */
    const std::uint32_t poc_type = b.ue();
    if (poc_type == 0)
        b.ue();
    else if (poc_type == 1)
    {
        b.u(1);
        b.se();
        b.se();
        const std::uint32_t cycle = b.ue();
        for (std::uint32_t i = 0; i < cycle && i < 256 && !b.overrun(); ++i)
            b.se();
    }
    const int refs = static_cast<int>(b.ue());
    b.u(1);
    const int width_mbs = static_cast<int>(b.ue()) + 1;
    const int height_units = static_cast<int>(b.ue()) + 1;
    const bool frame_mbs_only = b.u(1);
    if (!frame_mbs_only)
        b.u(1);
    b.u(1);
    if (b.u(1))
    {
        b.ue();
        b.ue();
        b.ue();
        b.ue();
    }
    const int height_mbs = height_units * (frame_mbs_only ? 1 : 2);
    int dpb = std::min(h264_max_dpb_mbs(level, constraints & 0x10) / (width_mbs * height_mbs), 16);
    if (b.u(1)) /* vui_parameters_present_flag */
    {
        if (b.u(1) && b.u(8) == 255)
            b.u(32);
        if (b.u(1))
            b.u(1);
        if (b.u(1))
        {
            b.u(4);
            if (b.u(1))
                b.u(24);
        }
        if (b.u(1))
        {
            b.ue();
            b.ue();
        }
        if (b.u(1))
        {
            b.u(32);
            b.u(32);
            b.u(1);
        }
        const bool nal_hrd = b.u(1);
        if (nal_hrd)
            h264_hrd(b);
        const bool vcl_hrd = b.u(1);
        if (vcl_hrd)
            h264_hrd(b);
        if (nal_hrd || vcl_hrd)
            b.u(1);
        b.u(1);
        if (b.u(1)) /* bitstream_restriction_flag */
        {
            b.u(1);
            b.ue();
            b.ue();
            b.ue();
            b.ue();
            b.ue(); /* max_num_reorder_frames */
            const int declared = static_cast<int>(b.ue());
            if (!b.overrun())
                dpb = declared;
        }
    }
    if (b.overrun() || width_mbs <= 0 || height_mbs <= 0)
        return shape;
    shape.valid = true;
    shape.width = width_mbs * 16;
    shape.height = height_mbs * 16;
    shape.dpb = std::clamp(std::max(dpb, refs), 1, 16);
    shape.level = level;
    return shape;
}

inline StreamShape hevc_sps(const std::vector<std::uint8_t> &sps) noexcept
{
    StreamShape shape;
    if (sps.size() < 16)
        return shape;
    Bits b(sps.data(), sps.size());
    b.u(4);
    const unsigned sub_layers = b.u(3);
    b.u(1);
    b.skip(88); /* general profile space, tier, profile, compatibility, constraints */
    const int level = static_cast<int>(b.u(8));
    bool profile_present[8] = {}, level_present[8] = {};
    for (unsigned i = 0; i < sub_layers; ++i)
    {
        profile_present[i] = b.u(1);
        level_present[i] = b.u(1);
    }
    if (sub_layers > 0)
        for (unsigned i = sub_layers; i < 8; ++i)
            b.u(2);
    for (unsigned i = 0; i < sub_layers; ++i)
    {
        if (profile_present[i])
            b.skip(88);
        if (level_present[i])
            b.u(8);
    }
    b.ue(); /* sps_seq_parameter_set_id */
    if (b.ue() == 3)
        b.u(1);
    const int width = static_cast<int>(b.ue());
    const int height = static_cast<int>(b.ue());
    if (b.u(1))
    {
        b.ue();
        b.ue();
        b.ue();
        b.ue();
    }
    shape.bit_depth = static_cast<int>(b.ue()) + 8;
    b.ue();
    b.ue(); /* log2_max_pic_order_cnt_lsb_minus4 */
    const bool all_layers = b.u(1);
    int dpb = 0;
    for (unsigned i = all_layers ? 0 : sub_layers; i <= sub_layers && !b.overrun(); ++i)
    {
        dpb = static_cast<int>(b.ue()) + 1;
        b.ue();
        b.ue();
    }
    if (b.overrun() || width <= 0 || height <= 0 || dpb <= 0)
        return {};
    shape.valid = true;
    shape.width = width;
    shape.height = height;
    shape.dpb = std::min(dpb, 16);
    shape.level = level;
    return shape;
}
} // namespace detail

/* The first sequence parameter set in an Annex-B access unit, or invalid. */
inline StreamShape stream_shape(const std::uint8_t *data, std::size_t bytes, bool hevc)
{
    StreamShape shape;
    detail::each_nal(data, bytes,
                     [&](const std::uint8_t *nal, std::size_t size)
                     {
                         if (size < 2)
                             return false;
                         if (!hevc && (nal[0] & 0x1f) == 7)
                         {
                             shape = detail::h264_sps(detail::rbsp(nal + 1, size - 1));
                             return true;
                         }
                         /* Layer 0 only: a Dolby Vision enhancement layer has its own SPS. */
                         if (hevc && ((nal[0] >> 1) & 0x3f) == 33 &&
                             ((nal[0] & 1) << 5 | nal[1] >> 3) == 0)
                         {
                             shape = detail::hevc_sps(detail::rbsp(nal + 2, size - 2));
                             return true;
                         }
                         return false;
                     });
    return shape;
}

} // namespace slopfin::video

#endif
