/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
namespace slopfin::gpu_menu
{
// One clipped compute dispatch. Positions and coverage stay at output resolution.
enum Kind : int
{
    fill,
    gradient_x,
    gradient_y,
    image,
    mask,
    mask_gradient,
    shadow,
    arc,
    stroke,
    scanout,
    glyph_batch /* internal: a run of atlas glyphs */
};
struct Command
{
    int clip[4]{};              // x0,y0,x1,y1
    int rect[4]{};              // x,y,w,h
    int info[4]{};              // kind, surface stride, radius, extra
    std::uint32_t colours[4]{}; // from,to,opacity,reserved
    float source[4]{};          // source origin and step (pixels)
    int texture[4]{};           // width,height,byte mask,unused
    float effect[4]{};
};
static_assert(sizeof(Command) == 112);
/* What the GPU menu path did since the last take_stats(), for the trace. */
struct Stats
{
    std::uint64_t gpu_wait_us = 0;    /* submit until the fence signalled */
    std::uint64_t gpu_busy_us = 0;    /* first command to last, by the GPU's own clock */
    std::uint64_t gpu_latency_us = 0; /* submission to finish, as the flip thread saw it */
    std::uint64_t latency_samples = 0;
    std::uint64_t submits = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t barriers = 0;       /* dispatches that had to wait for earlier ones */
    std::uint64_t glyphs = 0;         /* text and icon masks drawn through batches */
    std::uint64_t cpu_syncs = 0;      /* CPU drawing forced a GPU round trip */
    std::uint64_t hashed_bytes = 0;   /* source pixels checksummed or compared */
    std::uint64_t uploaded_bytes = 0; /* source pixels copied to new GPU buffers */
    std::uint64_t readback_bytes = 0; /* frame copied back to the CPU */
};
#if defined(SLOPFIN_GPU_MENU)
bool initialize(int width, int height) noexcept;
void shutdown() noexcept;
void enable(bool value) noexcept;
bool active() noexcept;
/* A nonzero `id` names immutable pixels (gfx::Bitmap::id): found without hashing. */
bool draw(const Command &, const void *pixels = nullptr, std::size_t bytes = 0,
          std::uint64_t id = 0) noexcept;
// Flush queued GPU work before a CPU primitive/capture uses the shared surface.
void cpu_access(std::uint32_t *pixels) noexcept;
Stats take_stats() noexcept;
void finish(std::uint32_t *pixels) noexcept;
/* Records the frame's conversion into the display buffer and submits it.
   Returns the submission's slot, or -1 when it could not. With `wait` it
   has finished on return; without, wait_submission(slot) must follow, on any
   thread, before the buffer is flipped. */
int present(std::uint32_t *frame, std::size_t bytes, bool wait) noexcept;
void wait_submission(int slot) noexcept;
#else
inline bool initialize(int, int) noexcept
{
    return false;
}
inline void shutdown() noexcept
{
}
inline void enable(bool) noexcept
{
}
inline bool active() noexcept
{
    return false;
}
inline bool draw(const Command &, const void * = nullptr, std::size_t = 0,
                 std::uint64_t = 0) noexcept
{
    return false;
}
inline void cpu_access(std::uint32_t *) noexcept
{
}
inline Stats take_stats() noexcept
{
    return {};
}
inline void finish(std::uint32_t *) noexcept
{
}
inline int present(std::uint32_t *, std::size_t, bool) noexcept
{
    return -1;
}
inline void wait_submission(int) noexcept
{
}
#endif
} // namespace slopfin::gpu_menu
