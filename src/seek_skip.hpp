/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A seek starts decoding at the keyframe before the target. Pictures before
 * the target are never shown, and those no later picture references need not
 * be decoded at all: in a B-frame stream that is most of them, which is most
 * of the wait after a seek.
 *
 * H.264 marks such a picture with nal_ref_idc 0. HEVC marks sub-layer
 * non-reference pictures with an even NAL type below 16, but a picture of a
 * higher temporal sub-layer may still reference one, so they are only left
 * out of streams the SPS declares to have a single sub-layer.
 */
#ifndef SLOPFIN_SEEK_SKIP_HPP
#define SLOPFIN_SEEK_SKIP_HPP

#include "video_sps.hpp"

namespace slopfin::video
{

struct SkipState
{
    bool single_layer = false; /* HEVC: known only once an SPS has been seen */
};

/* Whether this Annex-B access unit can go undecoded. */
inline bool disposable(const std::uint8_t *data, std::size_t bytes, bool hevc,
                       SkipState &state) noexcept
{
    bool result = false;
    detail::each_nal(data, bytes,
                     [&](const std::uint8_t *nal, std::size_t size)
                     {
                         if (!hevc)
                         {
                             const int type = nal[0] & 0x1f;
                             if (type != 1 && type != 5)
                                 return false;
                             result = type == 1 && (nal[0] & 0x60) == 0;
                             return true;
                         }
                         if (size < 3)
                             return false;
                         const int type = (nal[0] >> 1) & 0x3f;
                         if (type == 33) /* SPS: sps_max_sub_layers_minus1 */
                             state.single_layer = ((nal[2] >> 1) & 7) == 0;
                         if (type > 31)
                             return false;
                         result = state.single_layer && type < 16 && type % 2 == 0;
                         return true; /* the first slice decides */
                     });
    return result;
}

} // namespace slopfin::video

#endif
