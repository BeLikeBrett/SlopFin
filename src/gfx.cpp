/*
 * SlopFin - framebuffer display and 2D drawing.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The VideoOut setup follows ps5-native-app-boilerplate's proven sequence,
 * extended to a real double-buffered present loop.
 */

#include "gfx.hpp"
#include "hdr.hpp"
#include "video_scale.hpp"

#include "bigalloc.hpp"
#include "trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <pthread.h>
#include <string>

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceSystemServiceHideSplashScreen();
    int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                        const void *param);
    int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
    int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index,
                              std::uint32_t flip_mode, std::int64_t flip_argument);
    int sceVideoOutWaitVblank(std::int32_t handle);
    int sceKernelGettimeofday(void *timeval);
    int sceKernelCreateEqueue(void **queue, const char *name);
    int sceKernelDeleteEqueue(void *queue);
    int sceKernelWaitEqueue(void *queue, void *events, int count, int *received,
                            std::uint32_t *timeout_us);
    int sceVideoOutAddFlipEvent(void *queue, int handle, void *data);
    int sceVideoOutDeleteFlipEvent(void *queue, int handle);
    void *malloc(std::size_t size);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadJoin(void *thread, void **value);
    void *scePthreadSelf();
    int scePthreadGetaffinity(void *thread, std::uint64_t *mask);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
}

namespace
{
/* Opaque to us; the size is what sceVideoOutSetBufferAttribute2 expects. */
struct VideoAttribute
{
    std::uint8_t reserved[80];
};

struct VideoBuffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute,
                                               std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width,
                                               std::uint32_t height, std::uint64_t option,
                                               std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutUnregisterBuffers(std::int32_t handle, std::int32_t set_index);
extern "C" int sceVideoOutSubmitChangeBufferAttribute2(std::int32_t handle, std::int32_t set_index,
                                                       VideoAttribute *attribute, void *option);
extern "C" int sceVideoOutClose(std::int32_t handle);
extern "C" int sceVideoOutGetCurrentOutputMode_(std::int32_t handle, ...);
extern "C" int sceVideoOutGetResolutionStatus(std::int32_t handle, void *status);
extern "C" int sceVideoOutGetDeviceCapabilityInfo_(std::int32_t handle, ...);

/*
 * Display interrogation. These names are what libSceVideoOut exports; the
 * argument lists are not published, so each is declared variadic and called
 * with several shapes. Only a null or a buffer of our own is ever passed where
 * a pointer might be expected, so a wrong shape returns an error rather than
 * writing somewhere it should not.
 */
extern "C" int sceVideoOutGetVblankStatus(std::int32_t handle, ...);
extern "C" int sceVideoOutGetOutputStatus(std::int32_t handle, ...);
extern "C" int sceVideoOutGetMonitorInfo(std::int32_t handle, ...);
extern "C" int sceVideoOutGetHdmiMonitorInfo_(std::int32_t handle, ...);
extern "C" int sceVideoOutGetHdmiRawEdid_(std::int32_t handle, ...);
extern "C" int sceVideoOutGetDeviceInfoEx_(std::int32_t handle, ...);
extern "C" int sceVideoOutGetPortStatusInfo_(std::int32_t handle, ...);
extern "C" int sceVideoOutSysGetCurrentOutputMode(std::int32_t handle, ...);
extern "C" int sceSystemServiceGetHdrToneMapLuminance(void *luminance, ...);
extern "C" int sceSystemServiceGetRenderingMode(void *mode, ...);
extern "C" int sceVideoOutGetVideoOutModeByBusSpecifier_(std::int32_t bus, ...);
extern "C" int sceVideoOutGetPortStatusInfoByBusSpecifier_(std::int32_t bus, ...);
extern "C" int sceVideoOutInitializeOutputOptions(void *options, ...);
extern "C" int sceVideoOutConfigureOptionsInitialize_(void *options, ...);

extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index,
                                           std::int32_t buffer_index_start, VideoBuffer *buffers,
                                           std::int32_t buffer_count, VideoAttribute *attribute,
                                           std::int32_t category, void *option);

constexpr std::uint64_t kPixelFormatRgba8Srgb = UINT64_C(0x8000000022000000);
constexpr int kMemoryTypeWcGarlic = 3;
constexpr int kMapProtection = 0x33;
constexpr std::size_t kAlignment = 0x200000;
constexpr std::size_t kFrameCount = 2;
/* Nothing larger is attempted, whatever the console reports. */
constexpr int kMaxWidth = 3840;
constexpr int kMaxHeight = 2160;

/* Tiling: 128x128 blocks of 64 KiB, with a swizzle inside each block. */
constexpr unsigned kTile = 128;
constexpr std::size_t kTileWords = kTile * kTile;
/* Block counts follow the surface, so they are settled at start-up. */
unsigned g_blocks_across = 0;
unsigned g_block_rows = 0;
std::size_t g_frame_bytes = 0;

constexpr std::uint32_t tile_swizzle(unsigned x, unsigned y) noexcept
{
    return ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U) ^
           ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^ ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^
           ((x << 9) & 0xa000U);
}

void step(const char *name) noexcept
{
    if (std::FILE *file = std::fopen("/data/slopfin-step.txt", "wb"); file != nullptr)
    {
        (void)std::fputs(name, file);
        (void)std::fclose(file);
    }
}

int g_video = -1;
std::atomic<bool> g_hdr_output{false};
std::atomic<std::uint64_t> g_hdr_requested{0};
std::atomic<std::uint64_t> g_hdr_applied{0};
/* Set by /data/slopfin-sdr-only: SDR whatever the console's HDR setting says. */
bool g_sdr_only = false;
std::atomic<slopfin::gfx::SystemHdr> g_system_hdr{slopfin::gfx::SystemHdr::unknown};
slopfin::hdr::Compositor g_hdr_compositor;
/* The surface being drawn into; equal to the logical size until start-up. */
int g_phys_w = slopfin::gfx::kWidth;
int g_phys_h = slopfin::gfx::kHeight;
void *g_flip_queue = nullptr;
std::int64_t g_flip_sequence = 0;
std::array<std::uint32_t *, kFrameCount> g_frames{};
std::size_t g_back = 0;
bool g_reuse_frame = false;
std::uint64_t g_surface_version = 0;
std::array<std::uint64_t, kFrameCount> g_frame_versions{};

/*
 * Drawing happens in ordinary cached memory. Video memory is write-combined,
 * so reading it back to alpha-blend would be crippling; the finished frame is
 * copied out once per present, in tile order, which is the access pattern
 * write-combining is built for.
 */
std::uint32_t *g_staging = nullptr;
/*
 * Where drawing goes. Normally the staging buffer; while a caller is composing
 * something it means to keep, a buffer of its own. Every primitive strides by
 * g_phys_w, so a target is always a whole surface.
 */
std::uint32_t *g_target = nullptr;

/* For each word in a tile, the pixel it came from. Built once at startup. */
bool g_capture_pending = false;
/* What the pending capture should dump: a box in physical pixels, and how far
   to shrink it. All zeroes means the whole surface. */
int g_capture_box[4] = {0, 0, 0, 0};
int g_capture_shrink = 2;

std::array<std::uint16_t, kTileWords> g_tile_x{};
std::array<std::uint16_t, kTileWords> g_tile_y{};

void build_tile_table() noexcept
{
    for (unsigned y = 0; y < kTile; ++y)
    {
        for (unsigned x = 0; x < kTile; ++x)
        {
            const std::uint32_t word = tile_swizzle(x, y) >> 2;
            g_tile_x[word] = static_cast<std::uint16_t>(x);
            g_tile_y[word] = static_cast<std::uint16_t>(y);
        }
    }
}

/*
 * One thread's share of the tiled copy: every `stride`th row of blocks. The
 * copy is the single most expensive thing the app does each frame -- at 4K it
 * is eight million words gathered through the tile swizzle -- and it is
 * embarrassingly parallel, so it is spread across a few threads rather than
 * deciding the frame rate on its own.
 */
void copy_slice(std::uint32_t *frame, unsigned first_row, unsigned stride) noexcept
{
    for (unsigned by = first_row; by < g_block_rows; by += stride)
    {
        for (unsigned bx = 0; bx < g_blocks_across; ++bx)
        {
            std::uint32_t *out =
                frame + (static_cast<std::size_t>(by) * g_blocks_across + bx) * kTileWords;
            const unsigned origin_x = bx * kTile;
            const unsigned origin_y = by * kTile;
            for (std::size_t word = 0; word < kTileWords; ++word)
            {
                const unsigned x = origin_x + g_tile_x[word];
                const unsigned y = origin_y + g_tile_y[word];
                out[word] =
                    (x < static_cast<unsigned>(g_phys_w) && y < static_cast<unsigned>(g_phys_h))
                        ? (g_hdr_output
                               ? g_staging[static_cast<std::size_t>(y) * g_phys_w + x]
                               : slopfin::gfx::scanout_pixel(
                                     g_staging[static_cast<std::size_t>(y) * g_phys_w + x]))
                        : (g_hdr_output ? slopfin::hdr::pack(0, 0, 0) : 0xff000000u);
            }
        }
    }
}

/* Split expensive blits across row workers; synchronization uses condition variables. */
using RowJob = void (*)(void *context, unsigned first, unsigned stride);

/*
 * Three, plus whichever thread calls run_rows. Five made every frame ten times
 * slower rather than faster: this process gets four cores, so six threads meant
 * every barrier waited on a context switch.
 */
constexpr unsigned kPoolWorkers = 3;
pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_pool_start = PTHREAD_COND_INITIALIZER;
pthread_cond_t g_pool_finished = PTHREAD_COND_INITIALIZER;
std::array<void *, kPoolWorkers> g_pool_threads{};
RowJob g_pool_job = nullptr;
void *g_pool_context = nullptr;
std::uint64_t g_pool_generation = 0;
unsigned g_pool_outstanding = 0;
unsigned g_pool_started = 0;
/* Every core this process may run on, read before the render thread pins
   itself to one of them. Nothing can ask for it afterwards: a thread's own
   affinity is all the kernel reports, and the render thread's is one core. */
std::uint64_t g_process_cores = 0;
bool g_pool_stop = false;

void *pool_worker(void *argument) noexcept
{
    const auto index = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(argument));
    std::uint64_t seen = 0;
    (void)pthread_mutex_lock(&g_pool_mutex);
    for (;;)
    {
        while (!g_pool_stop && g_pool_generation == seen)
            (void)pthread_cond_wait(&g_pool_start, &g_pool_mutex);
        if (g_pool_stop)
            break;
        seen = g_pool_generation;
        const RowJob job = g_pool_job;
        void *context = g_pool_context;
        const unsigned stride = g_pool_started + 1;
        (void)pthread_mutex_unlock(&g_pool_mutex);

        if (job != nullptr)
            job(context, index + 1, stride);

        (void)pthread_mutex_lock(&g_pool_mutex);
        if (--g_pool_outstanding == 0)
            (void)pthread_cond_signal(&g_pool_finished);
    }
    (void)pthread_mutex_unlock(&g_pool_mutex);
    return nullptr;
}

void start_pool() noexcept
{
    /*
     * A new thread inherits the creator's affinity, and with every worker on
     * the one core the render thread is already using, splitting the work
     * changed nothing at all: four threads took exactly as long as one, and
     * six took ten times longer. Each worker gets its own core out of whatever
     * this process is allowed.
     */
    std::uint64_t allowed = 0;
    if (scePthreadGetaffinity(scePthreadSelf(), &allowed) != 0)
        allowed = 0;
    g_process_cores = allowed;
    std::array<unsigned, 16> cores{};
    unsigned core_count = 0;
    for (unsigned bit = 0; bit < 16 && core_count < cores.size(); ++bit)
    {
        if ((allowed & (UINT64_C(1) << bit)) != 0)
            cores[core_count++] = bit;
    }
    slopfin::trace::mark("gfx: affinity 0x" + std::to_string(allowed) + " cores " +
                         std::to_string(core_count));

    for (unsigned i = 0; i < kPoolWorkers; ++i)
    {
        void *thread = nullptr;
        if (scePthreadCreate(&thread, nullptr, pool_worker,
                             reinterpret_cast<void *>(static_cast<std::uintptr_t>(i)),
                             "slopfin-rows") != 0)
            break;
        if (core_count > 1)
        {
            const unsigned core = cores[(i + 1) % core_count];
            (void)scePthreadSetaffinity(thread, UINT64_C(1) << core);
        }
        g_pool_threads[i] = thread;
        ++g_pool_started;
    }
    /* The caller keeps the first core to itself. */
    if (core_count > 1)
        (void)scePthreadSetaffinity(scePthreadSelf(), UINT64_C(1) << cores[0]);
    slopfin::trace::mark("gfx: row workers " + std::to_string(g_pool_started));
}

void stop_pool() noexcept
{
    if (g_pool_started == 0)
        return;
    (void)pthread_mutex_lock(&g_pool_mutex);
    g_pool_stop = true;
    (void)pthread_cond_broadcast(&g_pool_start);
    (void)pthread_mutex_unlock(&g_pool_mutex);
    /* Joined, not merely asked to stop: a worker still writing into the frames
       when they are unregistered is writing into memory that has gone. */
    for (unsigned i = 0; i < g_pool_started; ++i)
        (void)scePthreadJoin(g_pool_threads[i], nullptr);
    g_pool_started = 0;
}

/* Runs `job` across the pool and this thread, and returns when all are done. */
void run_rows(RowJob job, void *context) noexcept
{
    if (g_pool_started == 0)
    {
        job(context, 0, 1);
        return;
    }
    (void)pthread_mutex_lock(&g_pool_mutex);
    g_pool_job = job;
    g_pool_context = context;
    g_pool_outstanding = g_pool_started;
    ++g_pool_generation;
    (void)pthread_cond_broadcast(&g_pool_start);
    (void)pthread_mutex_unlock(&g_pool_mutex);

    job(context, 0, g_pool_started + 1);

    (void)pthread_mutex_lock(&g_pool_mutex);
    while (g_pool_outstanding != 0)
        (void)pthread_cond_wait(&g_pool_finished, &g_pool_mutex);
    (void)pthread_mutex_unlock(&g_pool_mutex);
}

void copy_to_frame(std::uint32_t *frame) noexcept
{
    run_rows([](void *context, unsigned first, unsigned stride)
             { copy_slice(static_cast<std::uint32_t *>(context), first, stride); }, frame);
}

/* Clip rectangles are kept in physical pixels, converted as they are pushed. */
/*
 * A logical offset added to everything drawn, so a whole page can be moved
 * without every screen knowing about it: the page transitions slide the
 * content in. Sizes are differences of two converted edges, so the offset
 * cancels out of every width and height.
 */
int g_origin_x = 0;
int g_origin_y = 0;

struct Clip
{
    int x0 = 0;
    int y0 = 0;
    int x1 = slopfin::gfx::kWidth;
    int y1 = slopfin::gfx::kHeight;
};
std::array<Clip, 8> g_clip_stack{};
std::size_t g_clip_depth = 0;

const Clip &clip() noexcept
{
    return g_clip_stack[g_clip_depth];
}

/*
 * Divides by 255 with rounding, without dividing. Three integer divisions per
 * pixel is what made a full-screen scrim take a fifth of a second at 4K: a
 * 32-bit divide is twenty-odd cycles and does not pipeline, so the blend was
 * spending more time in the divider than everywhere else put together.
 * Exact for every product of two bytes.
 */
inline std::uint32_t over_255(std::uint32_t value) noexcept
{
    const std::uint32_t rounded = value + 128u;
    return (rounded + (rounded >> 8)) >> 8;
}

/* Source-over blend of a straight-alpha colour onto an opaque destination. */
inline void blend_pixel(std::uint32_t *dst, slopfin::gfx::Color src) noexcept
{
    if (g_hdr_output)
    {
        *dst = g_hdr_compositor.blend(*dst, src);
        return;
    }
    const std::uint32_t a = src >> 24;
    if (a == 0)
        return;
    if (a == 255)
    {
        *dst = src | 0xff000000u;
        return;
    }
    const std::uint32_t inv = 255u - a;
    const std::uint32_t d = *dst;
    const std::uint32_t r = over_255(((src >> 16) & 0xffu) * a + ((d >> 16) & 0xffu) * inv);
    const std::uint32_t g = over_255(((src >> 8) & 0xffu) * a + ((d >> 8) & 0xffu) * inv);
    const std::uint32_t b = over_255((src & 0xffu) * a + (d & 0xffu) * inv);
    *dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * How much of the pixel at (px, py) a rounded rectangle covers, 0 to 1. The
 * straight edges land on whole pixels and need nothing; only the corner arcs
 * are softened, across a single pixel. A hard inside-or-out test there is what
 * made every card, pill and focus ring look chewed at the corners.
 */
inline float rounded_coverage(int px, int py, int rx, int ry, int rw, int rh, int rr) noexcept
{
    if (px < rx || py < ry || px >= rx + rw || py >= ry + rh)
        return 0.0f;
    if (rr <= 0)
        return 1.0f;
    const bool left = px < rx + rr;
    const bool right = px >= rx + rw - rr;
    const bool top = py < ry + rr;
    const bool bottom = py >= ry + rh - rr;
    if (!((left || right) && (top || bottom)))
        return 1.0f;
    const auto radius = static_cast<float>(rr);
    const float cx = left ? static_cast<float>(rx) + radius : static_cast<float>(rx + rw) - radius;
    const float cy = top ? static_cast<float>(ry) + radius : static_cast<float>(ry + rh) - radius;
    const float dx = static_cast<float>(px) + 0.5f - cx;
    const float dy = static_cast<float>(py) + 0.5f - cy;
    return std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
}

/* The same colour at a fraction of its alpha, for a partly covered pixel. */
inline slopfin::gfx::Color at_coverage(slopfin::gfx::Color color, float coverage) noexcept
{
    const auto alpha =
        static_cast<std::uint32_t>(static_cast<float>(color >> 24) * coverage + 0.5f);
    return (color & 0x00ffffffu) | (alpha << 24);
}

/*
 * One bilinear sample, in 16.16 source coordinates. Nearest-neighbour is what
 * made artwork look like it had been resized in a paint program: a poster
 * drawn a few percent off its own size loses and doubles whole rows.
 */
inline std::uint32_t sample_bilinear(const slopfin::gfx::Bitmap &src, std::int64_t u,
                                     std::int64_t v) noexcept
{
    const int last_x = src.width - 1;
    const int last_y = src.height - 1;
    const int x0 = std::clamp(static_cast<int>(u >> 16), 0, last_x);
    const int y0 = std::clamp(static_cast<int>(v >> 16), 0, last_y);
    const int x1 = std::min(x0 + 1, last_x);
    const int y1 = std::min(y0 + 1, last_y);
    const auto fx = static_cast<std::uint32_t>((u >> 8) & 0xff);
    const auto fy = static_cast<std::uint32_t>((v >> 8) & 0xff);
    const std::uint32_t w11 = fx * fy;
    const std::uint32_t w10 = (256u - fx) * fy;
    const std::uint32_t w01 = fx * (256u - fy);
    const std::uint32_t w00 = (256u - fx) * (256u - fy);

    const std::uint32_t *top = src.pixels + static_cast<std::size_t>(y0) * src.width;
    const std::uint32_t *bottom = src.pixels + static_cast<std::size_t>(y1) * src.width;
    const std::uint32_t a = top[x0];
    const std::uint32_t b = top[x1];
    const std::uint32_t c = bottom[x0];
    const std::uint32_t d = bottom[x1];

    std::uint32_t out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        const std::uint32_t value = (((a >> shift) & 0xffu) * w00 + ((b >> shift) & 0xffu) * w01 +
                                     ((c >> shift) & 0xffu) * w10 + ((d >> shift) & 0xffu) * w11) >>
                                    16;
        out |= (value & 0xffu) << shift;
    }
    return out;
}
} // namespace

namespace slopfin::gfx
{
bool hdr_output() noexcept
{
    return g_hdr_output;
}
std::uint32_t to_hdr_pixel(Color color) noexcept
{
    return g_hdr_compositor.blend(slopfin::hdr::pack(0, 0, 0), color | 0xff000000u);
}
/* PS5 HDR output status: byte 4 is 2 for On When Supported and 1 for Off (FW 8.20). */
SystemHdr read_system_hdr() noexcept
{
    alignas(8) std::uint8_t status[64] = {};
    if (g_video < 0 || sceVideoOutGetOutputStatus(g_video, status) != 0)
        return SystemHdr::unknown;
    switch (status[4])
    {
    case 2:
        return SystemHdr::on;
    case 1:
        return SystemHdr::off;
    default:
        return SystemHdr::unknown;
    }
}
SystemHdr system_hdr() noexcept
{
    return g_system_hdr.load(std::memory_order_acquire);
}
bool sdr_only() noexcept
{
    return g_sdr_only;
}
void request_hdr_output(bool enabled) noexcept
{
    auto previous = g_hdr_requested.load(std::memory_order_relaxed);
    while (!g_hdr_requested.compare_exchange_weak(previous, ((previous & ~1ULL) + 2) | enabled,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed))
    {
    }
}
bool hdr_request_pending() noexcept
{
    return g_hdr_requested.load(std::memory_order_acquire) !=
           g_hdr_applied.load(std::memory_order_acquire);
}

/*
 * The pixel-space bodies. Everything public takes logical coordinates and
 * converts its edges before calling these, so the tested per-pixel logic is
 * written once and runs in real pixels.
 */
void fill_rect_phys(int x, int y, int w, int h, Color color) noexcept;
void rounded_rect_phys(int x, int y, int w, int h, int radius, Color color) noexcept;
void stroke_rounded_rect_phys(int x, int y, int w, int h, int radius, int thickness,
                              Color color) noexcept;
void blit_cover_phys(const Bitmap &src, int x, int y, int w, int h, int radius,
                     std::uint8_t alpha) noexcept;
void blit_video_phys(const Bitmap &src, int x, int y, int w, int h) noexcept;

bool initialize() noexcept
{
    build_tile_table();

    (void)sceSystemServiceHideSplashScreen();
    g_video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (g_video < 0)
        return false;
    /*
     * What the console is sending to the display right now. Registering a
     * surface of a different size is what makes the display blank on launch
     * and exit while it switches modes, and an upscaled surface is soft.
     * Layout (PS4 SDK): fullWidth, fullHeight, paneWidth, paneHeight (u32),
     * refreshRate (u64), screenSizeInInch (float), flags (u16).
     */
    {
        alignas(8) std::uint8_t status[64] = {};
        const int result = sceVideoOutGetResolutionStatus(g_video, status);
        std::uint32_t words[4] = {};
        std::memcpy(words, status, sizeof(words));
        std::uint64_t refresh = 0;
        std::memcpy(&refresh, status + 16, sizeof(refresh));
        slopfin::trace::mark("gfx: output status 0x" + std::to_string(result) + " full " +
                             std::to_string(words[0]) + "x" + std::to_string(words[1]) + " pane " +
                             std::to_string(words[2]) + "x" + std::to_string(words[3]) +
                             " refresh " + std::to_string(refresh));
    }
    if (sceKernelCreateEqueue(&g_flip_queue, "slopfin-flips") != 0 ||
        sceVideoOutAddFlipEvent(g_flip_queue, g_video, nullptr) != 0)
        return false;

    /*
     * Draw at the display's own resolution. A surface smaller than the output
     * is scaled by the console and then again by the panel, which is both soft
     * and, when the sizes differ, a mode change that blanks the display while
     * it settles. A marker file overrides it, which is how the cost of each
     * resolution was measured.
     */
    /* Read console output dimensions; image-sized buffers must use flexible memory. */
    /* Render the UI at 1920x1080 to limit CPU blit bandwidth; logical layout remains unchanged. */
    g_phys_w = slopfin::gfx::kWidth;
    g_phys_h = slopfin::gfx::kHeight;
    if (std::FILE *choice = std::fopen("/data/slopfin-res", "rb"); choice != nullptr)
    {
        int w = 0;
        int h = 0;
        if (std::fscanf(choice, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= kMaxWidth &&
            h <= kMaxHeight)
        {
            g_phys_w = w;
            g_phys_h = h;
        }
        (void)std::fclose(choice);
    }
    g_blocks_across = (static_cast<unsigned>(g_phys_w) + kTile - 1) / kTile;
    g_block_rows = (static_cast<unsigned>(g_phys_h) + kTile - 1) / kTile;
    /* Each frame starts on the allocation alignment, so round its size up. */
    const std::size_t tiled = static_cast<std::size_t>(g_blocks_across) * g_block_rows *
                              kTileWords * sizeof(std::uint32_t);
    g_frame_bytes = ((tiled + kAlignment - 1) / kAlignment) * kAlignment;
    const std::size_t memory_bytes = g_frame_bytes * kFrameCount;
    slopfin::trace::mark("gfx: surface " + std::to_string(g_phys_w) + "x" +
                         std::to_string(g_phys_h) + " frame " + std::to_string(g_frame_bytes) +
                         " bytes");

    slopfin::trace::mark("gfx: step queue ok");
    step("surface chosen");
    const std::size_t pool = sceKernelGetDirectMemorySize();
    if (pool < memory_bytes)
    {
        slopfin::trace::mark("gfx: direct memory pool too small");
        return false;
    }
    slopfin::trace::mark("gfx: step pool " + std::to_string(pool / (1024 * 1024)) + " MiB");

    std::int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool), memory_bytes, kAlignment,
                                      kMemoryTypeWcGarlic, &physical) < 0)
        return false;

    slopfin::trace::mark("gfx: step allocated");
    step("pool checked");
    void *mapped = nullptr;
    if (sceKernelMapDirectMemory(&mapped, memory_bytes, kMapProtection, 0, physical, kAlignment) <
        0)
    {
        slopfin::trace::mark("gfx: mapping the frames failed");
        return false;
    }
    slopfin::trace::mark("gfx: step mapped");
    step("mapped");

    auto *bytes = static_cast<std::uint8_t *>(mapped);
    std::array<VideoBuffer, kFrameCount> buffers{};
    for (std::size_t i = 0; i < kFrameCount; ++i)
    {
        g_frames[i] = reinterpret_cast<std::uint32_t *>(bytes + i * g_frame_bytes);
        buffers[i] = VideoBuffer{g_frames[i], nullptr, nullptr, nullptr};
    }

    (void)sceVideoOutSetFlipRate(g_video, 0);

    /*
     * Buffers can only be registered once: re-registering on a live port is
     * refused, and so is unregistering it. So the format has to be chosen here,
     * at start-up, and a wider format is only attempted when a marker file asks
     * for it. Whatever happens, the 8-bit sRGB format is registered if the
     * wider one is refused, because a port with no buffers cannot draw at all.
     */
    std::uint64_t chosen = kPixelFormatRgba8Srgb;
    if (std::FILE *want = std::fopen("/data/slopfin-hdr", "rb"); want != nullptr)
    {
        char text[32] = {};
        const std::size_t read = std::fread(text, 1, sizeof(text) - 1, want);
        (void)std::fclose(want);
        if (read > 0)
        {
            const std::uint64_t requested = std::strtoull(text, nullptr, 16);
            if (requested == slopfin::hdr::kPixelFormat)
                chosen = requested;
        }
    }

    /*
     * Not every size is accepted: this console takes 1920x1080 and 3840x2160
     * but refuses 2560x1440, so a surface matching an unusual panel can fail.
     * A refusal falls back rather than leaving the app with no surface, which
     * is a dead process recoverable only by a marker file over FTP.
     */
    VideoAttribute attribute{};
    sceVideoOutSetBufferAttribute2(&attribute, chosen, 0, static_cast<std::uint32_t>(g_phys_w),
                                   static_cast<std::uint32_t>(g_phys_h), 0, 0, 0);
    int registered = sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers.data(),
                                                 static_cast<std::int32_t>(buffers.size()),
                                                 &attribute, 0, nullptr);
    if (registered < 0 && (g_phys_w != kWidth || g_phys_h != kHeight))
    {
        slopfin::trace::mark("gfx: that surface was refused, falling back to 1920x1080");
        g_phys_w = kWidth;
        g_phys_h = kHeight;
        g_blocks_across = (static_cast<unsigned>(g_phys_w) + kTile - 1) / kTile;
        g_block_rows = (static_cast<unsigned>(g_phys_h) + kTile - 1) / kTile;
        VideoAttribute smaller{};
        sceVideoOutSetBufferAttribute2(&smaller, chosen, 0, static_cast<std::uint32_t>(g_phys_w),
                                       static_cast<std::uint32_t>(g_phys_h), 0, 0, 0);
        registered = sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers.data(),
                                                 static_cast<std::int32_t>(buffers.size()),
                                                 &smaller, 0, nullptr);
    }
    if (std::FILE *note = std::fopen("/data/slopfin-gfx-caps.txt", "wb"); note != nullptr)
    {
        std::fprintf(note, "requested format 0x%llx -> %s (0x%x)\n",
                     static_cast<unsigned long long>(chosen),
                     registered >= 0 ? "ACCEPTED" : "refused", registered);
        (void)std::fclose(note);
    }
    if (registered < 0 && chosen != kPixelFormatRgba8Srgb)
    {
        chosen = kPixelFormatRgba8Srgb;
        VideoAttribute fallback{};
        sceVideoOutSetBufferAttribute2(&fallback, kPixelFormatRgba8Srgb, 0,
                                       static_cast<std::uint32_t>(g_phys_w),
                                       static_cast<std::uint32_t>(g_phys_h), 0, 0, 0);
        registered = sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers.data(),
                                                 static_cast<std::int32_t>(buffers.size()),
                                                 &fallback, 0, nullptr);
    }
    if (registered < 0)
        return false;

    g_hdr_output = chosen == slopfin::hdr::kPixelFormat;
    g_hdr_compositor.initialize();
    if (std::FILE *sdr_only = std::fopen("/data/slopfin-sdr-only", "rb"); sdr_only != nullptr)
    {
        g_sdr_only = true;
        (void)std::fclose(sdr_only);
    }
    slopfin::trace::mark(g_hdr_output ? "gfx: HDR10 PQ compositor enabled"
                                      : "gfx: SDR compositor enabled");

    slopfin::trace::mark("gfx: step registered " + std::to_string(registered));
    step("registered");

    /*
     * Drawing happens here, not in video memory, and only now is the size
     * final: a surface that was refused has already fallen back. 33 MB at 4K,
     * which the process heap cannot hand out, so it comes from flexible
     * memory.
     */
    const std::size_t staging_bytes =
        static_cast<std::size_t>(g_phys_w) * g_phys_h * sizeof(std::uint32_t);
    g_staging = static_cast<std::uint32_t *>(slopfin::bigalloc::allocate(staging_bytes));
    slopfin::trace::mark("gfx: step staging " + std::to_string(staging_bytes / (1024 * 1024)) +
                         " MiB");
    step("staging alloc");
    if (g_staging == nullptr)
        return false;
    step("clearing staging");
    std::memset(g_staging, 0, staging_bytes);
    step("staging cleared");
    slopfin::trace::mark("gfx: staging cleared");
    for (std::size_t i = 0; i < kFrameCount; ++i)
    {
        step(i == 0 ? "clearing frame 0" : "clearing frame 1");
        std::memset(g_frames[i], 0, g_frame_bytes);
        step(i == 0 ? "frame 0 cleared" : "frame 1 cleared");
        slopfin::trace::mark("gfx: frame " + std::to_string(i) + " cleared");
    }
    start_pool();
    step("initialize done");
    g_back = 0;
    g_clip_stack[0] = Clip{0, 0, g_phys_w, g_phys_h};
    g_clip_depth = 0;
    return true;
}

int physical_width() noexcept
{
    return g_phys_w;
}

int physical_height() noexcept
{
    return g_phys_h;
}

/*
 * Edges are converted, never a position plus a size: two rectangles that share
 * an edge in logical space still share it exactly after scaling, so no seam
 * appears between them when the scale is fractional.
 */
int to_physical_x(int x) noexcept
{
    return ((x + g_origin_x) * g_phys_w) / kWidth;
}

int to_physical_y(int y) noexcept
{
    return ((y + g_origin_y) * g_phys_h) / kHeight;
}

void set_origin(int dx, int dy) noexcept
{
    g_origin_x = dx;
    g_origin_y = dy;
}

int to_physical_size(int v) noexcept
{
    return (v * g_phys_h) / kHeight;
}

int to_logical_size(int v) noexcept
{
    return (v * kHeight) / g_phys_h;
}

void fill_rect(int x, int y, int w, int h, Color color) noexcept
{
    const int x0 = to_physical_x(x);
    const int y0 = to_physical_y(y);
    fill_rect_phys(x0, y0, to_physical_x(x + w) - x0, to_physical_y(y + h) - y0, color);
}

void rounded_rect(int x, int y, int w, int h, int radius, Color color) noexcept
{
    const int x0 = to_physical_x(x);
    const int y0 = to_physical_y(y);
    rounded_rect_phys(x0, y0, to_physical_x(x + w) - x0, to_physical_y(y + h) - y0,
                      to_physical_size(radius), color);
}

/* Render a fading arc for the loading indicator. */
void arc(int lcx, int lcy, int lradius, int lthickness, float from, float sweep,
         Color color) noexcept
{
    const int cx = to_physical_x(lcx);
    const int cy = to_physical_y(lcy);
    const auto radius = static_cast<float>(to_physical_size(lradius));
    const auto half = static_cast<float>(to_physical_size(lthickness)) * 0.5f;
    if (radius <= 0.0f || half <= 0.0f || sweep <= 0.0f)
        return;

    const Clip &c = clip();
    const int reach = static_cast<int>(radius + half) + 2;
    const int x0 = std::max(cx - reach, c.x0);
    const int y0 = std::max(cy - reach, c.y0);
    const int x1 = std::min(cx + reach, c.x1);
    const int y1 = std::min(cy + reach, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const auto strength = static_cast<float>(color >> 24);
    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const float dy = static_cast<float>(py) + 0.5f - static_cast<float>(cy);
        for (int px = x0; px < x1; ++px)
        {
            const float dx = static_cast<float>(px) + 0.5f - static_cast<float>(cx);
            const float distance = std::sqrt(dx * dx + dy * dy);
            /* Inside the outer edge and outside the inner one, softened across
               a pixel on both. */
            const float coverage =
                std::clamp(std::min(distance - (radius - half), (radius + half) - distance) + 0.5f,
                           0.0f, 1.0f);
            if (coverage <= 0.0f)
                continue;

            float fade = 1.0f;
            if (sweep < 1.0f)
            {
                /* Turns clockwise from twelve o'clock. */
                float turn = std::atan2(dx, -dy) * 0.15915494f;
                if (turn < 0.0f)
                    turn += 1.0f;
                float behind = from - turn;
                behind -= std::floor(behind);
                if (behind > sweep)
                    continue;
                fade = 1.0f - behind / sweep;
            }
            const auto alpha = static_cast<std::uint8_t>(strength * coverage * fade + 0.5f);
            if (alpha == 0)
                continue;
            blend_pixel(row + px, (color & 0x00ffffffu) | (static_cast<Color>(alpha) << 24));
        }
    }
}

void drop_shadow(int lx, int ly, int lw, int lh, int radius, int spread,
                 std::uint8_t strength) noexcept
{
    if (spread <= 0 || lw <= 0 || lh <= 0)
        return;
    const int x = to_physical_x(lx);
    const int y = to_physical_y(ly);
    const int w = to_physical_x(lx + lw) - x;
    const int h = to_physical_y(ly + lh) - y;
    const auto r = static_cast<float>(to_physical_size(radius));
    const int reach = std::max(1, to_physical_size(spread));
    /* Skip the interior when drawing an offset shadow so translucent artwork is not darkened. */
    const int drop = std::max(1, reach / 5);

    /* Blend the shadow halo in one pass using rounded-rectangle distance. */
    const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
    const float cy = static_cast<float>(y + drop) + static_cast<float>(h) * 0.5f;
    const float half_w = static_cast<float>(w) * 0.5f - r;
    const float half_h = static_cast<float>(h) * 0.5f - r;
    const float span = static_cast<float>(reach);

    const Clip &c = clip();
    const int x0 = std::max(x - reach, c.x0);
    const int y0 = std::max(y - reach + drop, c.y0);
    const int x1 = std::min(x + w + reach, c.x1);
    const int y1 = std::min(y + h + reach + drop, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const float dy = std::fabs(static_cast<float>(py) + 0.5f - cy);
        const float outside_y = std::max(dy - half_h, 0.0f);
        /* Rows level with the card only have a halo at its two sides. */
        const bool band = outside_y <= 0.0f;
        for (int px = x0; px < x1; ++px)
        {
            if (band)
            {
                const int inner_left = x + reach;
                const int inner_right = x + w - reach;
                if (px >= inner_left && px < inner_right)
                {
                    px = inner_right - 1;
                    continue;
                }
            }
            const float dx = std::fabs(static_cast<float>(px) + 0.5f - cx);
            const float outside_x = std::max(dx - half_w, 0.0f);
            const float distance = std::sqrt(outside_x * outside_x + outside_y * outside_y) - r;
            if (distance <= 0.0f || distance >= span)
                continue;
            const float fade = 1.0f - distance / span;
            const auto alpha =
                static_cast<std::uint8_t>(static_cast<float>(strength) * fade * fade + 0.5f);
            if (alpha == 0)
                continue;
            blend_pixel(row + px, rgba(0x00, 0x00, 0x00, alpha));
        }
    }
}

void stroke_rounded_rect(int x, int y, int w, int h, int radius, int thickness,
                         Color color) noexcept
{
    const int x0 = to_physical_x(x);
    const int y0 = to_physical_y(y);
    stroke_rounded_rect_phys(x0, y0, to_physical_x(x + w) - x0, to_physical_y(y + h) - y0,
                             to_physical_size(radius), std::max(1, to_physical_size(thickness)),
                             color);
}

/*
 * The whole image scaled into the box, alpha and all. Distinct from
 * blit_cover, which crops to fill: a show's lettering has to arrive complete
 * or it is not its lettering any more.
 */
void blit_scaled(const Bitmap &src, int lx, int ly, int lw, int lh) noexcept
{
    if (!src.valid() || lw <= 0 || lh <= 0)
        return;
    const int x = to_physical_x(lx);
    const int y = to_physical_y(ly);
    const int w = to_physical_x(lx + lw) - x;
    const int h = to_physical_y(ly + lh) - y;
    if (w <= 0 || h <= 0)
        return;

    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const std::int64_t step_x = (static_cast<std::int64_t>(src.width) << 16) / w;
    const std::int64_t step_y = (static_cast<std::int64_t>(src.height) << 16) / h;
    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const std::int64_t v = static_cast<std::int64_t>(py - y) * step_y + step_y / 2 - 32768;
        std::int64_t u = static_cast<std::int64_t>(x0 - x) * step_x + step_x / 2 - 32768;
        for (int px = x0; px < x1; ++px, u += step_x)
            blend_pixel(row + px, sample_bilinear(src, u, v));
    }
}

void blit_cover(const Bitmap &src, int x, int y, int w, int h, int radius,
                std::uint8_t alpha) noexcept
{
    const int x0 = to_physical_x(x);
    const int y0 = to_physical_y(y);
    blit_cover_phys(src, x0, y0, to_physical_x(x + w) - x0, to_physical_y(y + h) - y0,
                    to_physical_size(radius), alpha);
}

void blit_video(const Bitmap &src, int x, int y, int w, int h) noexcept
{
    const int x0 = to_physical_x(x);
    const int y0 = to_physical_y(y);
    blit_video_phys(src, x0, y0, to_physical_x(x + w) - x0, to_physical_y(y + h) - y0);
}

/*
 * Hand the display back. The app never did this, so whatever output mode the
 * port was left in outlived the process; probing a BT.2020 PQ buffer format
 * left the HDMI output in HDR while the app went on sending 8-bit sRGB, which
 * shows on the television as a washed-out blue cast.
 */
void shutdown() noexcept
{
    /* Before the frames are unregistered, so no worker is still writing into
       them. */
    stop_pool();
    if (g_video < 0)
        return;
    if (g_flip_queue != nullptr)
    {
        (void)sceVideoOutDeleteFlipEvent(g_flip_queue, g_video);
        (void)sceKernelDeleteEqueue(g_flip_queue);
        g_flip_queue = nullptr;
    }
    (void)sceVideoOutUnregisterBuffers(g_video, 0);
    (void)sceVideoOutClose(g_video);
    g_video = -1;
}

std::uint32_t *back_buffer() noexcept
{
    return g_target != nullptr ? g_target : g_staging;
}

bool push_target(std::uint32_t *pixels) noexcept
{
    if (pixels == nullptr || g_target != nullptr)
        return false;
    g_target = pixels;
    return true;
}

void pop_target() noexcept
{
    g_target = nullptr;
}

void blit_surface_height(const std::uint32_t *pixels, int logical_height) noexcept
{
    if (pixels == nullptr)
        return;
    struct SurfaceJob
    {
        const std::uint32_t *source;
        int rows;
    } job{pixels, std::clamp(to_physical_y(logical_height), 0, g_phys_h)};
    run_rows(
        [](void *context, unsigned first, unsigned stride)
        {
            const auto &job = *static_cast<const SurfaceJob *>(context);
            std::uint32_t *frame = back_buffer();
            for (int py = static_cast<int>(first); py < job.rows; py += static_cast<int>(stride))
            {
                const std::size_t at = static_cast<std::size_t>(py) * g_phys_w;
                std::copy_n(job.source + at, g_phys_w, frame + at);
            }
        },
        &job);
}

void blit_surface(const std::uint32_t *pixels) noexcept
{
    blit_surface_height(pixels, kHeight);
}

std::size_t surface_words() noexcept
{
    return static_cast<std::size_t>(g_phys_w) * g_phys_h;
}

bool write_display(const std::uint32_t *frame, const char *path) noexcept;
bool capture_region(const char *path, int x, int y, int w, int h, int shrink) noexcept;

void reuse_frame() noexcept
{
    g_reuse_frame = true;
}

void present() noexcept
{
    if (!g_reuse_frame)
        ++g_surface_version;
    g_reuse_frame = false;
    if (g_frame_versions[g_back] != g_surface_version)
    {
        copy_to_frame(g_frames[g_back]);
        g_frame_versions[g_back] = g_surface_version;
    }
    /* Finish write-combined stores before the display engine reads them. */
    __asm__ volatile("sfence" ::: "memory");
    /*
     * Capturing the framebuffer from the marker handler compared it against a
     * staging buffer one frame newer, which made an identical picture look 76
     * percent different. Both are taken here instead, from the same frame, so
     * any disagreement is a real fault in the tiled copy rather than timing.
     */
    if (g_capture_pending)
    {
        g_capture_pending = false;
        write_display(g_frames[g_back], "/data/slopfin-display.bin");
        (void)capture_region("/data/slopfin-frame.bin", g_capture_box[0], g_capture_box[1],
                             g_capture_box[2], g_capture_box[3], g_capture_shrink);
    }
    const int submitted =
        sceVideoOutSubmitFlip(g_video, static_cast<std::int32_t>(g_back), 1, ++g_flip_sequence);
    if (submitted != 0)
    {
        slopfin::trace::mark("gfx: flip submission failed");
        (void)sceVideoOutWaitVblank(g_video);
        return;
    }
    /* A vblank is not a completion fence. Wait for this port's actual flip
       before advancing the back buffer, as the PS5 SDL backend does. */
    alignas(8) std::uint8_t event[32]{}; // FreeBSD kevent
    int received = 0;
    while (sceKernelWaitEqueue(g_flip_queue, event, 1, &received, nullptr) != 0 || received != 1)
        (void)sceVideoOutWaitVblank(g_video);
    g_back = (g_back + 1) % kFrameCount;
}

/*
 * Capture what the display actually receives, by reading the tiled framebuffer
 * back and undoing the swizzle. The ordinary capture reads the staging buffer,
 * which is what the app meant to draw; when the two disagree the fault is in
 * the copy or the surface format, and only this one can show it.
 */
bool request_capture(int x, int y, int w, int h, int shrink) noexcept
{
    /*
     * Deferred to present, where the staging buffer and the tiled framebuffer
     * describe the same frame. Writing the dump here instead compared a
     * framebuffer against a staging buffer one frame newer, which made an
     * identical picture look 76 percent different.
     */
    g_capture_box[0] = x;
    g_capture_box[1] = y;
    g_capture_box[2] = w;
    g_capture_box[3] = h;
    g_capture_shrink = shrink;
    g_capture_pending = true;
    return true;
}

/*
 * What the console is actually sending to the television. The app's pixels
 * reach the framebuffer intact -- staging and the tiled surface compare
 * identical -- so a picture that looks wrong on the screen has to be explained
 * by the output mode, not by anything drawn.
 */
void report_output_mode() noexcept
{
    std::FILE *file = std::fopen("/data/slopfin-output-mode.txt", "wb");
    if (file == nullptr)
        return;

    /* Layouts are not published, so dump generous zeroed blocks as words. */
    std::uint32_t mode[32] = {};
    const int mode_result = sceVideoOutGetCurrentOutputMode_(g_video, mode);
    std::fprintf(file, "sceVideoOutGetCurrentOutputMode = 0x%x\n", mode_result);
    for (int i = 0; i < 12; ++i)
        std::fprintf(file, "  mode[%02d] = 0x%08x (%u)\n", i, mode[i], mode[i]);

    std::uint32_t caps[64] = {};
    const int caps_result = sceVideoOutGetDeviceCapabilityInfo_(g_video, caps);
    std::fprintf(file, "\nsceVideoOutGetDeviceCapabilityInfo = 0x%x\n", caps_result);
    for (int i = 0; i < 12; ++i)
        std::fprintf(file, "  caps[%02d] = 0x%08x (%u)\n", i, caps[i], caps[i]);

    (void)std::fclose(file);
}

namespace
{
/* A probe writes into its own buffer, so a call that ignores the shape it was
   given cannot reach anything else. */
alignas(16) std::uint8_t g_probe[8192];

void probe_line(std::FILE *file, const char *name, const char *shape, int result) noexcept
{
    std::fprintf(file, "%-44s %-22s = 0x%08x", name, shape, static_cast<unsigned>(result));
    std::size_t written = 0;
    for (std::size_t i = 0; i < sizeof(g_probe); ++i)
        if (g_probe[i] != 0)
            written = i + 1;
    if (written == 0)
    {
        std::fputs("   (wrote nothing)\n", file);
    }
    else
    {
        std::fprintf(file, "   wrote %zu bytes\n", written);
        const std::size_t show = written < 96 ? written : 96;
        for (std::size_t i = 0; i < show; i += 16)
        {
            std::fprintf(file, "      %04zx ", i);
            for (std::size_t j = i; j < i + 16 && j < show; ++j)
                std::fprintf(file, "%02x ", g_probe[j]);
            std::fputc('\n', file);
        }
    }
    (void)std::fflush(file);
}

void save_if_edid(const char *path) noexcept
{
    static const std::uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    for (std::size_t at = 0; at + 128 <= sizeof(g_probe); ++at)
    {
        if (std::memcmp(g_probe + at, header, sizeof(header)) != 0)
            continue;
        if (std::FILE *out = std::fopen(path, "wb"); out != nullptr)
        {
            /* Base block plus any extensions the header counts. */
            const std::size_t blocks = 1u + g_probe[at + 126];
            std::size_t bytes = blocks * 128u;
            if (at + bytes > sizeof(g_probe))
                bytes = sizeof(g_probe) - at;
            (void)std::fwrite(g_probe + at, 1, bytes, out);
            (void)std::fclose(out);
        }
        return;
    }
}
} // namespace

/* Record display capabilities and measured refresh cadence. */
void report_display_info() noexcept
{
    std::FILE *file = std::fopen("/data/slopfin-display.txt", "wb");
    if (file == nullptr)
        return;

    struct
    {
        std::int64_t seconds;
        std::int64_t microseconds;
    } before{}, after{};

    /* The cadence the display is actually running at, measured rather than
       reported: 120 intervals is long enough to separate 59.94 from 60. */
    constexpr int kIntervals = 120;
    (void)sceVideoOutWaitVblank(g_video);
    (void)sceKernelGettimeofday(&before);
    for (int i = 0; i < kIntervals; ++i)
        (void)sceVideoOutWaitVblank(g_video);
    (void)sceKernelGettimeofday(&after);
    const double elapsed = static_cast<double>(after.seconds - before.seconds) +
                           static_cast<double>(after.microseconds - before.microseconds) / 1e6;
    std::fprintf(file, "measured vblank interval  %.4f ms over %d intervals\n",
                 elapsed * 1000.0 / kIntervals, kIntervals);
    std::fprintf(file, "measured refresh          %.3f Hz\n",
                 elapsed > 0.0 ? kIntervals / elapsed : 0.0);
    std::fprintf(file, "surface                   %dx%d\n\n", g_phys_w, g_phys_h);
    (void)std::fflush(file);

    const auto clear = []() { std::memset(g_probe, 0, sizeof(g_probe)); };

    clear();
    probe_line(file, "sceVideoOutGetVblankStatus", "(handle, buf)",
               sceVideoOutGetVblankStatus(g_video, g_probe));

    clear();
    probe_line(file, "sceVideoOutGetResolutionStatus", "(handle, buf)",
               sceVideoOutGetResolutionStatus(g_video, g_probe));

    clear();
    probe_line(file, "sceVideoOutGetCurrentOutputMode_", "(handle, buf)",
               sceVideoOutGetCurrentOutputMode_(g_video, g_probe));
    clear();
    probe_line(file, "sceVideoOutGetCurrentOutputMode_", "(handle, buf, 64)",
               sceVideoOutGetCurrentOutputMode_(g_video, g_probe, UINT64_C(64)));

    clear();
    probe_line(file, "sceVideoOutGetDeviceCapabilityInfo_", "(handle, buf)",
               sceVideoOutGetDeviceCapabilityInfo_(g_video, g_probe));
    clear();
    probe_line(file, "sceVideoOutGetDeviceCapabilityInfo_", "(handle, buf, 64)",
               sceVideoOutGetDeviceCapabilityInfo_(g_video, g_probe, UINT64_C(64)));

    clear();
    probe_line(file, "sceVideoOutGetOutputStatus", "(handle, buf)",
               sceVideoOutGetOutputStatus(g_video, g_probe));
    std::fprintf(file, "      byte 4 = %u: PS5 HDR setting %s\n", g_probe[4],
                 g_probe[4] == 2   ? "On When Supported"
                 : g_probe[4] == 1 ? "Off"
                                   : "not known");

    clear();
    probe_line(file, "sceVideoOutGetMonitorInfo", "(handle, buf)",
               sceVideoOutGetMonitorInfo(g_video, g_probe));

    clear();
    probe_line(file, "sceVideoOutGetDeviceInfoEx_", "(handle, buf, 256)",
               sceVideoOutGetDeviceInfoEx_(g_video, g_probe, UINT64_C(256)));

    clear();
    probe_line(file, "sceVideoOutGetPortStatusInfo_", "(handle, buf, 256)",
               sceVideoOutGetPortStatusInfo_(g_video, g_probe, UINT64_C(256)));

    clear();
    probe_line(file, "sceVideoOutGetHdmiMonitorInfo_", "(handle, buf, 4096)",
               sceVideoOutGetHdmiMonitorInfo_(g_video, g_probe, UINT64_C(4096)));
    save_if_edid("/data/slopfin-edid.bin");

    /* Added while looking for anything that follows the system HDR setting
       . All three only read. */
    clear();
    probe_line(file, "sceVideoOutSysGetCurrentOutputMode", "(handle, buf)",
               sceVideoOutSysGetCurrentOutputMode(g_video, g_probe));

    clear();
    probe_line(file, "sceSystemServiceGetHdrToneMapLuminance", "(buf)",
               sceSystemServiceGetHdrToneMapLuminance(g_probe));
    {
        float luminance[3] = {};
        std::memcpy(luminance, g_probe, sizeof(luminance));
        std::fprintf(file, "      as floats: max full frame %.1f, max %.1f, min %.4f nits\n",
                     luminance[0], luminance[1], luminance[2]);
    }

    clear();
    probe_line(file, "sceSystemServiceGetRenderingMode", "(buf)",
               sceSystemServiceGetRenderingMode(g_probe));

    /* Read-only display diagnostics. Do not probe output modes: configuration calls can wedge
     * VideoOut. */

    (void)std::fclose(file);
}

/*
 * What the console is sending the television. The refresh rate arrives as the
 * ABI's own enumeration rather than a number, so the ones it names are spelled
 * out here; anything else is reported as the raw value rather than guessed at.
 */
bool output_mode(int &width, int &height, double &hz) noexcept
{
    /* Use an oversized capability buffer because the ABI size is undocumented; undersized buffers
     * corrupt the stack. */
    alignas(8) std::uint8_t status[64] = {};
    if (sceVideoOutGetResolutionStatus(g_video, status) != 0)
        return false;
    std::uint32_t size[2] = {};
    std::memcpy(size, status, sizeof(size));
    std::uint64_t refresh = 0;
    std::memcpy(&refresh, status + 16, sizeof(refresh));
    width = static_cast<int>(size[0]);
    height = static_cast<int>(size[1]);
    switch (refresh)
    {
    case 1:
        hz = 23.976;
        break;
    case 2:
        hz = 50.0;
        break;
    case 3:
        hz = 59.94;
        break;
    case 13:
        hz = 119.88;
        break;
    case 35:
        hz = 89.91;
        break;
    default:
        hz = 0.0;
        break;
    }
    return true;
}

/* Do not call sceVideoOutConfigureOutputMode_ or SetFlipRate experimentally: tested calls froze
 * output. See docs/HDR.md. */

bool write_display(const std::uint32_t *frame, const char *path) noexcept
{
    if (frame == nullptr)
        return false;
    std::FILE *file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;

    constexpr int out_w = kWidth / 2;
    constexpr int out_h = kHeight / 2;
    static std::uint32_t row[out_w];
    for (int y = 0; y < out_h; ++y)
    {
        const unsigned source_y = static_cast<unsigned>((y * g_phys_h) / out_h);
        for (int x = 0; x < out_w; ++x)
        {
            const unsigned source_x = static_cast<unsigned>((x * g_phys_w) / out_w);
            const unsigned bx = source_x / kTile;
            const unsigned by = source_y / kTile;
            const std::uint32_t word = tile_swizzle(source_x % kTile, source_y % kTile) >> 2;
            row[x] =
                frame[(static_cast<std::size_t>(by) * g_blocks_across + bx) * kTileWords + word];
        }
        if (std::fwrite(row, sizeof(std::uint32_t), out_w, file) != out_w)
            break;
    }
    (void)std::fclose(file);
    return true;
}

bool capture_region(const char *path, int x, int y, int w, int h, int shrink) noexcept
{
    if (g_staging == nullptr)
        return false;
    /* An empty request means the whole surface. */
    if (w <= 0 || h <= 0)
    {
        x = 0;
        y = 0;
        w = g_phys_w;
        h = g_phys_h;
    }
    shrink = std::clamp(shrink, 1, 8);
    x = std::clamp(x, 0, std::max(0, g_phys_w - 1));
    y = std::clamp(y, 0, std::max(0, g_phys_h - 1));
    w = std::min(w, g_phys_w - x);
    h = std::min(h, g_phys_h - y);
    const int out_w = w / shrink;
    const int out_h = h / shrink;
    if (out_w <= 0 || out_h <= 0)
        return false;

    std::FILE *file = std::fopen(path, "wb");
    if (file == nullptr)
        return false;

    /*
     * A short header carries the size, so a dump can be a crop at its true
     * size or the whole surface shrunk to something that fits down the wire,
     * and the tools need not guess which. The old fixed 960x540 point-sample
     * could not answer the one question a screenshot is taken to answer here:
     * whether what is on screen is sharp.
     */
    const std::uint32_t header[4] = {UINT32_C(0x46504c53), static_cast<std::uint32_t>(out_w),
                                     static_cast<std::uint32_t>(out_h), g_hdr_output ? 1u : 0u};
    if (std::fwrite(header, sizeof(std::uint32_t), 4, file) != 4)
    {
        (void)std::fclose(file);
        return false;
    }

    static std::array<std::uint32_t, kMaxWidth> row{};
    const auto area = static_cast<std::uint32_t>(shrink * shrink);
    for (int oy = 0; oy < out_h; ++oy)
    {
        for (int ox = 0; ox < out_w; ++ox)
        {
            // Keep HDR codes intact: byte-wise averaging corrupts packed 10-bit samples.
            if (shrink == 1 || g_hdr_output)
            {
                row[static_cast<std::size_t>(ox)] =
                    g_staging[static_cast<std::size_t>(y + oy * shrink) * g_phys_w + x +
                              ox * shrink];
                continue;
            }
            /* Averaged, not point-sampled: a shrunk dump that drops pixels
               invents aliasing that is not on the screen. */
            std::uint32_t sums[4] = {};
            for (int sy = 0; sy < shrink; ++sy)
            {
                const std::uint32_t *source =
                    g_staging + static_cast<std::size_t>(y + oy * shrink + sy) * g_phys_w + x +
                    ox * shrink;
                for (int sx = 0; sx < shrink; ++sx)
                {
                    const std::uint32_t pixel = source[sx];
                    sums[0] += pixel & 0xffu;
                    sums[1] += (pixel >> 8) & 0xffu;
                    sums[2] += (pixel >> 16) & 0xffu;
                    sums[3] += (pixel >> 24) & 0xffu;
                }
            }
            row[static_cast<std::size_t>(ox)] = (sums[0] / area) | ((sums[1] / area) << 8) |
                                                ((sums[2] / area) << 16) | ((sums[3] / area) << 24);
        }
        if (std::fwrite(row.data(), sizeof(std::uint32_t), static_cast<std::size_t>(out_w), file) !=
            static_cast<std::size_t>(out_w))
            break;
    }
    (void)std::fclose(file);
    return true;
}

void push_clip(int lx, int ly, int lw, int lh) noexcept
{
    const int x = to_physical_x(lx);
    const int y = to_physical_y(ly);
    const int w = to_physical_x(lx + lw) - x;
    const int h = to_physical_y(ly + lh) - y;
    if (g_clip_depth + 1 >= g_clip_stack.size())
        return;
    const Clip &current = clip();
    Clip next;
    next.x0 = std::max(current.x0, x);
    next.y0 = std::max(current.y0, y);
    next.x1 = std::min(current.x1, x + w);
    next.y1 = std::min(current.y1, y + h);
    g_clip_stack[++g_clip_depth] = next;
}

void pop_clip() noexcept
{
    if (g_clip_depth > 0)
        --g_clip_depth;
}

void apply_pending_output_mode() noexcept
{
    const auto request = g_hdr_requested.load(std::memory_order_acquire);
    if (request == g_hdr_applied.load(std::memory_order_relaxed))
        return;

    /* Read on every request, so a change in Settings takes effect at the next
       title without restarting the app. This used to live inside clear(),
       which deadlocked playback setup whenever a decoded picture was already
       being blitted and no clear happened. */
    const SystemHdr setting = read_system_hdr();
    g_system_hdr.store(setting, std::memory_order_release);
    const bool desired = (request & 1) != 0 && !g_sdr_only && setting == SystemHdr::on;
    if (desired != g_hdr_output.load(std::memory_order_relaxed))
    {
        VideoAttribute attribute{};
        sceVideoOutSetBufferAttribute2(
            &attribute, desired ? slopfin::hdr::kPixelFormat : kPixelFormatRgba8Srgb, 0,
            static_cast<std::uint32_t>(g_phys_w), static_cast<std::uint32_t>(g_phys_h), 0, 0, 0);
        const int result = sceVideoOutSubmitChangeBufferAttribute2(g_video, 0, &attribute, nullptr);
        slopfin::trace::mark(std::string{"gfx: change to "} + (desired ? "HDR10" : "SDR") +
                             " result=" + std::to_string(result));
        if (result >= 0)
            g_hdr_output.store(desired, std::memory_order_release);
    }
    g_hdr_applied.store(request, std::memory_order_release);
}

void clear(Color color) noexcept
{
    apply_pending_output_mode();
    const std::uint32_t value =
        g_hdr_output ? g_hdr_compositor.blend(slopfin::hdr::pack(0, 0, 0), color | 0xff000000u)
                     : color | 0xff000000u;
    struct ClearJob
    {
        std::uint32_t value;
    } job{value};
    run_rows(
        [](void *context, unsigned first, unsigned stride)
        {
            const auto value = static_cast<const ClearJob *>(context)->value;
            std::uint32_t *frame = back_buffer();
            for (int py = static_cast<int>(first); py < g_phys_h; py += static_cast<int>(stride))
                std::fill_n(frame + static_cast<std::size_t>(py) * g_phys_w, g_phys_w, value);
        },
        &job);
}

/*
 * A flat fill, and the shape everything else here follows: rows outer, columns
 * inner, and split across the pool once the area is big enough to be worth the
 * handshake. A full-screen scrim at 4K is eight million blended pixels, and
 * three of them in a row is most of a frame on one core.
 */
struct FillJob
{
    int x0;
    int y0;
    int x1;
    int y1;
    Color color;
};

void fill_rows(void *context, unsigned first, unsigned stride) noexcept
{
    const FillJob &job = *static_cast<const FillJob *>(context);
    std::uint32_t *frame = back_buffer();
    /* An opaque fill is a run of identical words, which vectorises; going
       through the blend a pixel at a time costs twice as much for nothing. */
    const bool opaque = (job.color >> 24) == 255u && !g_hdr_output;
    const std::uint32_t value = job.color | 0xff000000u;
    for (int py = job.y0 + static_cast<int>(first); py < job.y1; py += static_cast<int>(stride))
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        if (opaque)
        {
            std::fill_n(row + job.x0, job.x1 - job.x0, value);
            continue;
        }
        for (int px = job.x0; px < job.x1; ++px)
            blend_pixel(row + px, job.color);
    }
}

/* Past this many pixels, handing the rows round pays for itself. */
constexpr long long kThreadAbove = 200000;

void fill_rect_phys(int x, int y, int w, int h, Color color) noexcept
{
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    FillJob job{x0, y0, x1, y1, color};
    if (static_cast<long long>(x1 - x0) * (y1 - y0) > kThreadAbove)
        run_rows(fill_rows, &job);
    else
        fill_rows(&job, 0, 1);
}

void rounded_rect_phys(int x, int y, int w, int h, int radius, Color color) noexcept
{
    radius = std::min(radius, std::min(w, h) / 2);
    if (radius <= 0)
    {
        fill_rect_phys(x, y, w, h, color);
        return;
    }
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const bool straight_row = py >= y + radius && py < y + h - radius;
        for (int px = x0; px < x1; ++px)
        {
            if (straight_row || (px >= x + radius && px < x + w - radius))
            {
                blend_pixel(row + px, color);
                continue;
            }
            const float coverage = rounded_coverage(px, py, x, y, w, h, radius);
            if (coverage <= 0.0f)
                continue;
            blend_pixel(row + px, coverage >= 1.0f ? color : at_coverage(color, coverage));
        }
    }
}

void stroke_rounded_rect_phys(int x, int y, int w, int h, int radius, int thickness,
                              Color color) noexcept
{
    if (thickness <= 0 || w <= 0 || h <= 0)
        return;
    radius = std::min(radius, std::min(w, h) / 2);

    /*
     * The outline is the outer rounded rectangle minus the one inset by the
     * thickness. Both are measured as coverage rather than tested for
     * containment, so the arcs are smooth and a one-pixel ring does not
     * disappear in places.
     */
    const int inner_x = x + thickness;
    const int inner_y = y + thickness;
    const int inner_w = w - thickness * 2;
    const int inner_h = h - thickness * 2;
    const int inner_r = std::max(0, radius - thickness);

    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        /* A row between the two horizontal bars only has the two uprights on
           it, so the middle of the box is skipped rather than tested. */
        const bool between = py >= y + radius + thickness && py < y + h - radius - thickness;
        for (int px = x0; px < x1; ++px)
        {
            if (between && px >= inner_x && px < inner_x + inner_w)
            {
                px = inner_x + inner_w - 1;
                continue;
            }
            float coverage = rounded_coverage(px, py, x, y, w, h, radius);
            if (coverage <= 0.0f)
                continue;
            if (inner_w > 0 && inner_h > 0)
                coverage -= rounded_coverage(px, py, inner_x, inner_y, inner_w, inner_h, inner_r);
            if (coverage <= 0.0f)
                continue;
            blend_pixel(row + px, coverage >= 1.0f ? color : at_coverage(color, coverage));
        }
    }
}

/* One colour between two, at t from 0 to 255. */
Color mix_colour(Color from, Color to, int t) noexcept
{
    const auto channel = [&](int shift)
    {
        const int a = static_cast<int>((from >> shift) & 0xffu);
        const int b = static_cast<int>((to >> shift) & 0xffu);
        return static_cast<std::uint32_t>(a + ((b - a) * t) / 255);
    };
    return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

struct GradientJob
{
    int x0;
    int y0;
    int x1;
    int y1;
    Color from;
    Color to;
    int ramp_origin;      /* where the ramp starts, before clipping */
    int ramp_span;        /* how far it runs, before clipping */
    const Color *columns; /* horizontal only: one colour per destination column */
};

void gradient_rows(void *context, unsigned first, unsigned stride) noexcept
{
    const GradientJob &job = *static_cast<const GradientJob *>(context);
    std::uint32_t *frame = back_buffer();
    for (int py = job.y0 + static_cast<int>(first); py < job.y1; py += static_cast<int>(stride))
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        if (job.columns == nullptr)
        {
            const Color value =
                mix_colour(job.from, job.to, ((py - job.ramp_origin) * 255) / job.ramp_span);
            for (int px = job.x0; px < job.x1; ++px)
                blend_pixel(row + px, value);
            continue;
        }
        for (int px = job.x0; px < job.x1; ++px)
            blend_pixel(row + px, job.columns[px - job.x0]);
    }
}

/*
 * Both gradients walk rows outer and columns inner. The horizontal one used to
 * fill one column at a time, touching every row of the surface for each of
 * nearly three thousand columns, which is the worst access pattern this
 * renderer had.
 */
void gradient(int lx, int ly, int lw, int lh, Color from, Color to, bool vertical) noexcept
{
    const int x = to_physical_x(lx);
    const int y = to_physical_y(ly);
    const int right = to_physical_x(lx + lw);
    const int bottom = to_physical_y(ly + lh);
    const Clip &c = clip();
    GradientJob job{};
    job.x0 = std::max(x, c.x0);
    job.y0 = std::max(y, c.y0);
    job.x1 = std::min(right, c.x1);
    job.y1 = std::min(bottom, c.y1);
    job.from = from;
    job.to = to;
    if (job.x0 >= job.x1 || job.y0 >= job.y1)
        return;

    /* The ramp is measured across the whole request, not the clipped part, so
       clipping never changes the colours. */
    static std::array<Color, kMaxWidth> columns{};
    if (vertical)
    {
        job.ramp_origin = y;
        job.ramp_span = std::max(1, bottom - y - 1);
    }
    else
    {
        const int span = std::max(1, right - x - 1);
        for (int px = job.x0; px < job.x1; ++px)
            columns[static_cast<std::size_t>(px - job.x0)] =
                mix_colour(from, to, ((px - x) * 255) / span);
        job.columns = columns.data();
    }

    if (static_cast<long long>(job.x1 - job.x0) * (job.y1 - job.y0) > kThreadAbove)
        run_rows(gradient_rows, &job);
    else
        gradient_rows(&job, 0, 1);
}

void vertical_gradient(int lx, int ly, int lw, int lh, Color top, Color bottom) noexcept
{
    gradient(lx, ly, lw, lh, top, bottom, true);
}

void horizontal_gradient(int lx, int ly, int lw, int lh, Color left, Color right) noexcept
{
    gradient(lx, ly, lw, lh, left, right, false);
}

void media_scrim(int height, Color tone, std::uint8_t base_alpha, int vertical_start,
                 int horizontal_width, std::uint8_t horizontal_alpha) noexcept
{
    const Clip &c = clip();
    const int y1 = std::min(to_physical_y(height), c.y1);
    const int y0 = std::max(0, c.y0);
    const int x0 = std::max(0, c.x0);
    const int x1 = std::min(g_phys_w, c.x1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const int vertical_y = to_physical_y(vertical_start);
    const int horizontal_end = std::clamp(to_physical_x(horizontal_width), 0, g_phys_w);
    const int horizontal_span = std::max(1, horizontal_end - 1);
    static std::array<std::uint8_t, kMaxWidth> horizontal{};
    for (int px = x0; px < x1; ++px)
    {
        horizontal[static_cast<std::size_t>(px - x0)] =
            px < horizontal_end
                ? static_cast<std::uint8_t>((static_cast<unsigned>(horizontal_alpha) *
                                             static_cast<unsigned>(horizontal_end - 1 - px)) /
                                            static_cast<unsigned>(horizontal_span))
                : 0;
    }

    struct ScrimJob
    {
        int x0, x1, y0, y1;
        int vertical_y;
        Color tone;
        std::uint8_t base_alpha;
        const std::uint8_t *horizontal;
    } job{x0, x1, y0, y1, vertical_y, tone & 0x00ffffffu, base_alpha, horizontal.data()};

    run_rows(
        [](void *context, unsigned first, unsigned stride)
        {
            const auto &job = *static_cast<const ScrimJob *>(context);
            std::uint32_t *frame = back_buffer();
            const int vertical_span = std::max(1, job.y1 - job.vertical_y - 1);
            const auto combine = [](std::uint32_t a, std::uint32_t b)
            { return a + b - over_255(a * b); };
            for (int py = job.y0 + static_cast<int>(first); py < job.y1;
                 py += static_cast<int>(stride))
            {
                const std::uint32_t vertical =
                    py < job.vertical_y
                        ? 0u
                        : static_cast<std::uint32_t>(
                              std::clamp(((py - job.vertical_y) * 255) / vertical_span, 0, 255));
                const std::uint32_t base_vertical = combine(job.base_alpha, vertical);
                std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
                for (int px = job.x0; px < job.x1; ++px)
                {
                    const std::uint32_t alpha = combine(
                        base_vertical, job.horizontal[static_cast<std::size_t>(px - job.x0)]);
                    blend_pixel(row + px, job.tone | (alpha << 24));
                }
            }
        },
        &job);
}

/* The position is logical; the image is drawn at its own pixel size, which is
   what a bitmap already rendered for this surface wants. */
void blit(const Bitmap &src, int lx, int ly, std::uint8_t alpha) noexcept
{
    if (!src.valid() || alpha == 0)
        return;
    const int x = to_physical_x(lx);
    const int y = to_physical_y(ly);
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + src.width, c.x1);
    const int y1 = std::min(y + src.height, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const std::uint32_t *srow = src.pixels + static_cast<std::size_t>(py - y) * src.width;
        for (int px = x0; px < x1; ++px)
        {
            std::uint32_t pixel = srow[px - x];
            if (alpha != 255)
            {
                const std::uint32_t a = over_255((pixel >> 24) * alpha);
                pixel = (pixel & 0x00ffffffu) | (a << 24);
            }
            blend_pixel(row + px, pixel);
        }
    }
}

void blend_mask_physical(int x, int y, int w, int h, const unsigned char *coverage,
                         Color color) noexcept
{
    if (coverage == nullptr || w <= 0 || h <= 0)
        return;
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const std::uint32_t base_alpha = color >> 24;
    const std::uint32_t rgb_only = color & 0x00ffffffu;
    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const unsigned char *src = coverage + static_cast<std::size_t>(py - y) * w;
        for (int px = x0; px < x1; ++px)
        {
            const std::uint32_t a = over_255(src[px - x] * base_alpha);
            if (a == 0)
                continue;
            blend_pixel(row + px, rgb_only | (a << 24));
        }
    }
}

void blend_mask_gradient_physical(int x, int y, int w, int h, const unsigned char *coverage,
                                  Color from, Color to) noexcept
{
    if (coverage == nullptr || w <= 0 || h <= 0)
        return;
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    const std::uint32_t base_alpha = from >> 24;
    std::uint32_t *frame = back_buffer();
    for (int py = y0; py < y1; ++py)
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const unsigned char *src = coverage + static_cast<std::size_t>(py - y) * w;
        for (int px = x0; px < x1; ++px)
        {
            const std::uint32_t a = over_255(src[px - x] * base_alpha);
            if (a == 0)
                continue;
            /* Swept across both axes, as the source artwork's own ramp is. */
            const int t = std::clamp(((px - x) * 192 + (py - y) * 64) / std::max(1, w), 0, 255);
            const auto mix = [&](int shift)
            {
                const int lo = static_cast<int>((from >> shift) & 0xffu);
                const int hi = static_cast<int>((to >> shift) & 0xffu);
                return static_cast<std::uint32_t>(lo + ((hi - lo) * t) / 255);
            };
            blend_pixel(row + px, (a << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0));
        }
    }
}

void blit_video_phys(const Bitmap &src, int x, int y, int w, int h) noexcept
{
    if (!src.valid() || w <= 0 || h <= 0)
        return;
    const Clip &c = clip();
    const int x0 = std::max(x, c.x0);
    const int y0 = std::max(y, c.y0);
    const int x1 = std::min(x + w, c.x1);
    const int y1 = std::min(y + h, c.y1);
    if (x0 >= x1 || y0 >= y1)
        return;

    /* One source column index per destination column, computed once. */
    static std::array<slopfin::video_scale::Sample, kMaxWidth> columns{};
    static int cached_w = -1;
    static int cached_src_w = -1;
    static int cached_x = -1;
    if (cached_w != w || cached_src_w != src.width || cached_x != x)
    {
        for (int i = 0; i < w && i < kMaxWidth; ++i)
        {
            columns[static_cast<std::size_t>(i)] = slopfin::video_scale::axis(i, src.width, w);
        }
        cached_w = w;
        cached_src_w = src.width;
        cached_x = x;
    }

    /*
     * A picture fills the screen, so at 4K this is eight million writes every
     * frame. Handed round the pool it is a quarter of that per core.
     */
    struct VideoJob
    {
        const Bitmap *src;
        const slopfin::video_scale::Sample *columns;
        int x;
        int y;
        int h;
        int x0;
        int y0;
        int x1;
        int y1;
        bool unscaled;
    } job{&src, columns.data(),
          x,    y,
          h,    x0,
          y0,   x1,
          y1,   w == src.width && h == src.height && src.hdr10 == g_hdr_output};

    run_rows(
        [](void *context, unsigned first, unsigned stride)
        {
            const VideoJob &v = *static_cast<const VideoJob *>(context);
            std::uint32_t *frame = back_buffer();
            for (int py = v.y0 + static_cast<int>(first); py < v.y1; py += static_cast<int>(stride))
            {
                const auto ys = slopfin::video_scale::axis(py - v.y, v.src->height, v.h);
                const std::uint32_t *source =
                    v.src->pixels + static_cast<std::size_t>(ys.first) * v.src->width;
                const std::uint32_t *lower =
                    v.src->pixels + static_cast<std::size_t>(ys.second) * v.src->width;
                std::uint32_t *out = frame + static_cast<std::size_t>(py) * g_phys_w;
                if (v.unscaled)
                {
                    std::copy_n(source + v.x0 - v.x, v.x1 - v.x0, out + v.x0);
                    continue;
                }
                for (int px = v.x0; px < v.x1; ++px)
                {
                    const auto xs = v.columns[px - v.x];
                    const auto top = slopfin::video_scale::mix(source[xs.first], source[xs.second],
                                                               xs.weight, v.src->hdr10);
                    const auto bottom = slopfin::video_scale::mix(lower[xs.first], lower[xs.second],
                                                                  xs.weight, v.src->hdr10);
                    const auto pixel =
                        slopfin::video_scale::mix(top, bottom, ys.weight, v.src->hdr10);
                    out[px] = v.src->hdr10   ? pixel
                              : g_hdr_output ? g_hdr_compositor.blend(slopfin::hdr::pack(0, 0, 0),
                                                                      pixel | 0xff000000u)
                                             : pixel | 0xff000000u;
                }
            }
        },
        &job);
}

/* What one row of a cover blit needs to know, shared by the threads doing it. */
struct CoverJob
{
    const Bitmap *src;
    int x;
    int y;
    int w;
    int h;
    int radius;
    std::uint8_t alpha;
    int x0;
    int y0;
    int x1;
    int y1;
    std::int64_t step_x;
    std::int64_t step_y;
    std::int64_t origin_u;
    std::int64_t origin_v;
};

/* Parallelize large image blits when their pixel count amortizes row-worker synchronization. */
std::int64_t g_cover_thread_above = 20000;

void cover_rows(void *context, unsigned first, unsigned stride) noexcept
{
    const CoverJob &job = *static_cast<const CoverJob *>(context);
    const Bitmap &src = *job.src;
    std::uint32_t *frame = back_buffer();
    for (int py = job.y0 + static_cast<int>(first); py < job.y1; py += static_cast<int>(stride))
    {
        std::uint32_t *row = frame + static_cast<std::size_t>(py) * g_phys_w;
        const std::int64_t v = job.origin_v + static_cast<std::int64_t>(py - job.y) * job.step_y;
        const bool straight_row =
            job.radius <= 0 || (py >= job.y + job.radius && py < job.y + job.h - job.radius);
        std::int64_t u = job.origin_u + static_cast<std::int64_t>(job.x0 - job.x) * job.step_x;
        for (int px = job.x0; px < job.x1; ++px, u += job.step_x)
        {
            float coverage = 1.0f;
            if (!straight_row && !(px >= job.x + job.radius && px < job.x + job.w - job.radius))
            {
                coverage = rounded_coverage(px, py, job.x, job.y, job.w, job.h, job.radius);
                if (coverage <= 0.0f)
                    continue;
            }
            std::uint32_t pixel = sample_bilinear(src, u, v);
            const auto weight =
                static_cast<std::uint32_t>(static_cast<float>(job.alpha) * coverage + 0.5f);
            if (weight != 255u)
            {
                const std::uint32_t a = over_255((pixel >> 24) * weight);
                pixel = (pixel & 0x00ffffffu) | (a << 24);
            }
            blend_pixel(row + px, pixel);
        }
    }
}

void blit_cover_phys(const Bitmap &src, int x, int y, int w, int h, int radius,
                     std::uint8_t alpha) noexcept
{
    if (!src.valid() || w <= 0 || h <= 0 || alpha == 0)
        return;

    /* Scale to cover, then centre-crop, so posters never letterbox. */
    const int scale_num = std::max(w * src.height, h * src.width);
    const int draw_w = std::max(1, (scale_num + src.height - 1) / src.height);
    const int draw_h = std::max(1, (scale_num + src.width - 1) / src.width);
    const int offset_x = (draw_w - w) / 2;
    const int offset_y = (draw_h - h) / 2;

    const Clip &c = clip();
    CoverJob job{};
    job.src = &src;
    job.x = x;
    job.y = y;
    job.w = w;
    job.h = h;
    job.alpha = alpha;
    job.radius = std::min(radius, std::min(w, h) / 2);
    job.x0 = std::max(x, c.x0);
    job.y0 = std::max(y, c.y0);
    job.x1 = std::min(x + w, c.x1);
    job.y1 = std::min(y + h, c.y1);
    if (job.x0 >= job.x1 || job.y0 >= job.y1)
        return;

    /*
     * 16.16 source coordinates, sampled from the centre of each destination
     * pixel. Sampling from its corner instead shifts the picture half a pixel
     * and shows up as a soft edge on one side only.
     */
    job.step_x = (static_cast<std::int64_t>(src.width) << 16) / draw_w;
    job.step_y = (static_cast<std::int64_t>(src.height) << 16) / draw_h;
    job.origin_u = static_cast<std::int64_t>(offset_x) * job.step_x + job.step_x / 2 - 32768;
    job.origin_v = static_cast<std::int64_t>(offset_y) * job.step_y + job.step_y / 2 - 32768;

    /*
     * Above this many pixels the blit is handed to the row pool. The figure
     * was chosen when a poster was the only thing drawn at that size and the
     * handshake was judged to cost more than it saved; a grid draws twelve of
     * them in a row, so benchmark() measures both sides of the decision rather
     * than assuming the old answer still holds.
     */
    if (static_cast<std::int64_t>(job.x1 - job.x0) * (job.y1 - job.y0) > g_cover_thread_above)
        run_rows(cover_rows, &job);
    else
        cover_rows(&job, 0, 1);
}

/*
 * Times each primitive at the surface's real size and writes the numbers out.
 * Guessing which one was slow cost several deploys and was wrong twice: the
 * divides were not it, and neither was the threading. Measure the primitive,
 * not the frame.
 */
void benchmark(const char *path) noexcept
{
    const auto clock = []()
    {
        struct
        {
            std::int64_t seconds;
            std::int64_t microseconds;
        } now{};
        (void)sceKernelGettimeofday(&now);
        return static_cast<std::uint64_t>(now.seconds) * 1000000u +
               static_cast<std::uint64_t>(now.microseconds);
    };
    const auto time_it = [&clock](auto &&body)
    {
        const std::uint64_t start = clock();
        for (int i = 0; i < 4; ++i)
            body();
        return (clock() - start) / 4;
    };

    /* A synthetic picture, so artwork need not be loaded to measure a blit. */
    Bitmap picture{};
    picture.width = 1920;
    picture.height = 1080;
    picture.pixels = static_cast<std::uint32_t *>(slopfin::bigalloc::allocate(
        static_cast<std::size_t>(picture.width) * picture.height * sizeof(std::uint32_t)));
    if (picture.pixels != nullptr)
    {
        for (int i = 0; i < picture.width * picture.height; ++i)
            picture.pixels[i] = 0xff000000u | static_cast<std::uint32_t>(i * 2654435761u);
    }

    const std::uint64_t clear_us = time_it([] { clear(rgb(0x10, 0x10, 0x10)); });
    const std::uint64_t opaque_us =
        time_it([] { fill_rect_phys(0, 0, g_phys_w, g_phys_h, rgb(0x20, 0x20, 0x20)); });
    const std::uint64_t blend_us =
        time_it([] { fill_rect_phys(0, 0, g_phys_w, g_phys_h, rgba(0x00, 0x00, 0x00, 0x80)); });
    FillJob single{0, 0, g_phys_w, g_phys_h, rgba(0x00, 0x00, 0x00, 0x80)};
    const std::uint64_t blend_one_us = time_it([&single] { fill_rows(&single, 0, 1); });
    const auto top = rgba(0x00, 0x00, 0x00, 0x00);
    const auto bottom = rgb(0x0d, 0x0d, 0x12);
    const std::uint64_t gradient_us =
        time_it([top, bottom] { vertical_gradient(0, 0, kWidth, kHeight, top, bottom); });
    const std::uint64_t cover_us =
        picture.pixels == nullptr
            ? 0
            : time_it([&picture] { blit_cover_phys(picture, 0, 0, g_phys_w, g_phys_h, 0, 255); });
    const std::uint64_t video_us =
        picture.pixels == nullptr
            ? 0
            : time_it([&picture] { blit_video_phys(picture, 0, 0, g_phys_w, g_phys_h); });
    /*
     * What a grid actually draws: twelve poster-sized blits in a row, not one
     * full-screen one. Measured on both sides of the threading threshold,
     * because that is the decision this is here to settle.
     */
    const auto twelve_cards = [&picture]()
    {
        for (int i = 0; i < 12; ++i)
        {
            const int x = to_physical_x(384 + (i % 5) * 266);
            const int y = to_physical_y(250 + (i / 5) * 456);
            blit_cover_phys(picture, x, y, to_physical_size(240), to_physical_size(360),
                            to_physical_size(10), 255);
        }
    };
    const std::int64_t saved_threshold = g_cover_thread_above;
    g_cover_thread_above = INT64_MAX; /* never threaded */
    const std::uint64_t cards_one_us = picture.pixels == nullptr ? 0 : time_it(twelve_cards);
    g_cover_thread_above = 0; /* always threaded */
    const std::uint64_t cards_pooled_us = picture.pixels == nullptr ? 0 : time_it(twelve_cards);
    g_cover_thread_above = saved_threshold;

    const std::uint64_t tiles_us = time_it([] { copy_to_frame(g_frames[g_back]); });

    if (picture.pixels != nullptr)
        slopfin::bigalloc::release(picture.pixels);

    if (std::FILE *file = std::fopen(path, "wb"); file != nullptr)
    {
        (void)std::fprintf(
            file,
            "surface %dx%d, %d row workers, microseconds each\n"
            "clear              %8llu\n"
            "fill opaque        %8llu\n"
            "fill blended       %8llu\n"
            "fill blended, 1 thread %8llu\n"
            "vertical gradient  %8llu\n"
            "blit_cover         %8llu\n"
            "12 cards, 1 thread %8llu\n"
            "12 cards, pooled   %8llu\n"
            "blit_video         %8llu\n"
            "copy_to_frame      %8llu\n",
            g_phys_w, g_phys_h, static_cast<int>(g_pool_started),
            static_cast<unsigned long long>(clear_us), static_cast<unsigned long long>(opaque_us),
            static_cast<unsigned long long>(blend_us),
            static_cast<unsigned long long>(blend_one_us),
            static_cast<unsigned long long>(gradient_us), static_cast<unsigned long long>(cover_us),
            static_cast<unsigned long long>(cards_one_us),
            static_cast<unsigned long long>(cards_pooled_us),
            static_cast<unsigned long long>(video_us), static_cast<unsigned long long>(tiles_us));
        (void)std::fclose(file);
    }
}

std::uint64_t process_cores() noexcept
{
    return g_process_cores;
}

} // namespace slopfin::gfx
