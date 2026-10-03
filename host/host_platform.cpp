/*
 * SlopFin - Linux host backend: the PS5 C ABI, implemented on SDL2 and POSIX.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every symbol here is one the console's own sources already call. Nothing in
 * src/ knows this file exists, which is the point: gfx.cpp, pad.cpp and
 * bigalloc.cpp are compiled unmodified for both targets.
 */

#include "host_platform.hpp"

#include <SDL.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace
{
/* ------------------------------------------------------------- tiling */

/* The console's swizzle, repeated here rather than shared, so the un-tiling is
   checked against the layout the hardware documents rather than against
   another copy of gfx.cpp's opinion of it. */
constexpr unsigned kTile = 128;
constexpr std::size_t kTileWords = kTile * kTile;

constexpr std::uint32_t tile_swizzle(unsigned x, unsigned y) noexcept
{
    return ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U) ^
           ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^ ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^
           ((x << 9) & 0xa000U);
}

/* ------------------------------------------------------------- display */

SDL_GameController *g_controller = nullptr;

struct Display
{
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    bool headless = false;
    bool quit = false;

    int width = 0;
    int height = 0;
    unsigned blocks_across = 0;
    unsigned block_rows = 0;

    /* Every registered frame, by index, as handed to RegisterBuffers. */
    std::vector<std::uint32_t *> frames;
    /* Scan-line order, rebuilt on each flip. */
    std::vector<std::uint32_t> linear;
    std::atomic<std::uint64_t> presented{0};
    bool fixed_step = false;
};

Display g_display;
slopfin::host::PadState g_pad;
std::mutex g_pad_mutex;

/* One table, built once: for each word in a tile, the pixel it holds. */
std::vector<std::uint16_t> g_untile_x;
std::vector<std::uint16_t> g_untile_y;

void build_untile_table() noexcept
{
    if (!g_untile_x.empty())
        return;
    g_untile_x.resize(kTileWords);
    g_untile_y.resize(kTileWords);
    for (unsigned y = 0; y < kTile; ++y)
        for (unsigned x = 0; x < kTile; ++x)
        {
            const std::uint32_t word = tile_swizzle(x, y) >> 2;
            g_untile_x[word] = static_cast<std::uint16_t>(x);
            g_untile_y[word] = static_cast<std::uint16_t>(y);
        }
}

void untile(const std::uint32_t *frame) noexcept
{
    build_untile_table();
    g_display.linear.resize(static_cast<std::size_t>(g_display.width) * g_display.height);
    for (unsigned by = 0; by < g_display.block_rows; ++by)
        for (unsigned bx = 0; bx < g_display.blocks_across; ++bx)
        {
            const std::uint32_t *in =
                frame + (static_cast<std::size_t>(by) * g_display.blocks_across + bx) * kTileWords;
            const unsigned origin_x = bx * kTile;
            const unsigned origin_y = by * kTile;
            for (std::size_t word = 0; word < kTileWords; ++word)
            {
                const unsigned x = origin_x + g_untile_x[word];
                const unsigned y = origin_y + g_untile_y[word];
                if (x < static_cast<unsigned>(g_display.width) &&
                    y < static_cast<unsigned>(g_display.height))
                    g_display.linear[static_cast<std::size_t>(y) * g_display.width + x] = in[word];
            }
        }
}

/* ---------------------------------------------------------------- keys */

/*
 * Keyboard to DualSense. The letters follow the physical arrangement of the
 * face buttons rather than their names, so the mapping can be used without
 * looking it up: J is the bottom button, L the right one.
 */
constexpr std::uint32_t kBtnUp = 0x0010u;
constexpr std::uint32_t kBtnRight = 0x0020u;
constexpr std::uint32_t kBtnDown = 0x0040u;
constexpr std::uint32_t kBtnLeft = 0x0080u;
constexpr std::uint32_t kBtnL1 = 0x0400u;
constexpr std::uint32_t kBtnR1 = 0x0800u;
constexpr std::uint32_t kBtnTriangle = 0x1000u;
constexpr std::uint32_t kBtnCircle = 0x2000u;
constexpr std::uint32_t kBtnCross = 0x4000u;
constexpr std::uint32_t kBtnSquare = 0x8000u;
constexpr std::uint32_t kBtnOptions = 0x0008u;
constexpr std::uint32_t kBtnL3 = 0x0002u;
constexpr std::uint32_t kBtnR3 = 0x0004u;
constexpr std::uint32_t kBtnL2 = 0x0100u;
constexpr std::uint32_t kBtnR2 = 0x0200u;
constexpr std::uint32_t kBtnTouchpad = 0x00100000u;

struct KeyMap
{
    SDL_Scancode key;
    std::uint32_t button;
};

constexpr KeyMap kKeys[] = {
    {SDL_SCANCODE_UP, kBtnUp},
    {SDL_SCANCODE_DOWN, kBtnDown},
    {SDL_SCANCODE_LEFT, kBtnLeft},
    {SDL_SCANCODE_RIGHT, kBtnRight},
    {SDL_SCANCODE_W, kBtnUp},
    {SDL_SCANCODE_S, kBtnDown},
    {SDL_SCANCODE_A, kBtnLeft},
    {SDL_SCANCODE_D, kBtnRight},
    {SDL_SCANCODE_RETURN, kBtnCross},
    {SDL_SCANCODE_Z, kBtnCross},
    {SDL_SCANCODE_BACKSPACE, kBtnCircle},
    {SDL_SCANCODE_X, kBtnCircle},
    {SDL_SCANCODE_C, kBtnSquare},
    {SDL_SCANCODE_V, kBtnTriangle},
    {SDL_SCANCODE_TAB, kBtnTriangle},
    {SDL_SCANCODE_Q, kBtnL1},
    {SDL_SCANCODE_E, kBtnR1},
    {SDL_SCANCODE_1, kBtnL2},
    {SDL_SCANCODE_2, kBtnR2},
    {SDL_SCANCODE_F, kBtnL3},
    {SDL_SCANCODE_G, kBtnR3},
    {SDL_SCANCODE_ESCAPE, kBtnOptions},
    {SDL_SCANCODE_P, kBtnOptions},
    {SDL_SCANCODE_T, kBtnTouchpad},
};

/* Wheel deflection decays, so one notch reads as a short push rather than a
   held stick. In frames. */
int g_wheel_frames = 0;
int g_wheel_direction = 0;
bool g_capture_requested = false;
} // namespace

namespace slopfin::host
{

bool open_display(const Options &options) noexcept
{
    g_display.headless = options.headless;
    g_display.fixed_step = options.fixed_step;
    if (options.headless)
        return true;

    /* A real pad is the point of a ten-foot interface, and testing one with a
       keyboard tests something else. SDL's game-controller layer maps a
       DualSense's face buttons onto A/B/X/Y in the order this app wants:
       A is cross, B circle, X square, Y triangle. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        std::fprintf(stderr, "slopfin: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    const int w = 1920 * options.scale_percent / 100;
    const int h = 1080 * options.scale_percent / 100;
    g_display.window =
        SDL_CreateWindow("SlopFin", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                         SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (g_display.window == nullptr)
    {
        std::fprintf(stderr, "slopfin: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_display.renderer = SDL_CreateRenderer(g_display.window, -1,
                                            SDL_RENDERER_ACCELERATED |
                                                (options.vsync ? SDL_RENDERER_PRESENTVSYNC : 0u));
    if (g_display.renderer == nullptr)
        g_display.renderer = SDL_CreateRenderer(g_display.window, -1, SDL_RENDERER_SOFTWARE);
    return g_display.renderer != nullptr;
}

void close_display() noexcept
{
    if (g_controller != nullptr)
    {
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    if (g_display.texture != nullptr)
        SDL_DestroyTexture(g_display.texture);
    if (g_display.renderer != nullptr)
        SDL_DestroyRenderer(g_display.renderer);
    if (g_display.window != nullptr)
        SDL_DestroyWindow(g_display.window);
    g_display.texture = nullptr;
    g_display.renderer = nullptr;
    g_display.window = nullptr;
    if (!g_display.headless)
        SDL_Quit();
}

PadState &pad_state() noexcept
{
    return g_pad;
}

bool pump_events() noexcept
{
    if (g_display.headless)
        return !g_display.quit;

    SDL_Event event;
    while (SDL_PollEvent(&event) != 0)
    {
        switch (event.type)
        {
        case SDL_QUIT:
            g_display.quit = true;
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.scancode == SDL_SCANCODE_F12)
                g_capture_requested = true;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (g_controller == nullptr)
            {
                g_controller = SDL_GameControllerOpen(event.cdevice.which);
                if (g_controller != nullptr)
                    std::fprintf(stderr, "slopfin: controller: %s\n",
                                 SDL_GameControllerName(g_controller));
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_controller != nullptr &&
                event.cdevice.which ==
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller)))
            {
                SDL_GameControllerClose(g_controller);
                g_controller = nullptr;
            }
            break;
        case SDL_MOUSEWHEEL:
            /* The wheel is the right stick: it is what scrolls a category. */
            g_wheel_direction = event.wheel.y > 0 ? -1 : 1;
            g_wheel_frames = 6;
            break;
        default:
            break;
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    std::uint32_t buttons = 0;
    for (const KeyMap &entry : kKeys)
        if (keys[entry.key] != 0)
            buttons |= entry.button;

    /* The pad is merged with the keyboard rather than replacing it, so a
       capture script driving injected presses still works with one plugged
       in. */
    int pad_left_x = 0;
    int pad_left_y = 0;
    int pad_right_y = 0;
    int pad_trigger_l = 0;
    int pad_trigger_r = 0;
    if (g_controller != nullptr)
    {
        struct PadMap
        {
            SDL_GameControllerButton button;
            std::uint32_t mask;
        };
        static const PadMap kPad[] = {
            {SDL_CONTROLLER_BUTTON_A, kBtnCross},
            {SDL_CONTROLLER_BUTTON_B, kBtnCircle},
            {SDL_CONTROLLER_BUTTON_X, kBtnSquare},
            {SDL_CONTROLLER_BUTTON_Y, kBtnTriangle},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, kBtnUp},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kBtnDown},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kBtnLeft},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kBtnRight},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kBtnL1},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kBtnR1},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, kBtnL3},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kBtnR3},
            {SDL_CONTROLLER_BUTTON_START, kBtnOptions},
            {SDL_CONTROLLER_BUTTON_TOUCHPAD, kBtnTouchpad},
        };
        for (const PadMap &entry : kPad)
            if (SDL_GameControllerGetButton(g_controller, entry.button) != 0)
                buttons |= entry.mask;

        /* The triggers are axes on a DualSense; past halfway counts as held,
           which is what the console's own L2/R2 handling expects. */
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
            buttons |= kBtnL2;
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
            buttons |= kBtnR2;

        pad_trigger_l = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        pad_trigger_r = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        pad_left_x = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        pad_left_y = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        pad_right_y = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);
    }

    std::lock_guard<std::mutex> guard(g_pad_mutex);
    g_pad.buttons = buttons;
    /* SDL reports a stick as -32768..32767; the console's sample is a byte
       centred on 128. */
    const auto to_byte = [](int axis)
    { return static_cast<std::uint8_t>(std::clamp(128 + axis / 258, 0, 255)); };
    g_pad.left_x = to_byte(pad_left_x);
    g_pad.left_y = to_byte(pad_left_y);
    int right_y = 0;
    if (keys[SDL_SCANCODE_I] != 0)
        right_y = -1;
    else if (keys[SDL_SCANCODE_K] != 0)
        right_y = 1;
    else if (g_wheel_frames > 0)
    {
        right_y = g_wheel_direction;
        --g_wheel_frames;
    }
    /* SDL reports a trigger as 0..32767; the console's sample is a byte. The
       keyboard's 1 and 2 stand in for a fully pressed trigger. */
    g_pad.trigger_l = static_cast<std::uint8_t>(
        std::clamp(keys[SDL_SCANCODE_1] != 0 ? 255 : pad_trigger_l / 129, 0, 255));
    g_pad.trigger_r = static_cast<std::uint8_t>(
        std::clamp(keys[SDL_SCANCODE_2] != 0 ? 255 : pad_trigger_r / 129, 0, 255));
    g_pad.right_x = 128;
    /* A real stick wins over the wheel when one is actually being pushed. */
    g_pad.right_y = pad_right_y > 6000 || pad_right_y < -6000
                        ? to_byte(pad_right_y)
                        : static_cast<std::uint8_t>(std::clamp(128 + right_y * 110, 0, 255));
    return !g_display.quit;
}

const std::uint32_t *scanout_pixels(int &width, int &height) noexcept
{
    width = g_display.width;
    height = g_display.height;
    return g_display.linear.empty() ? nullptr : g_display.linear.data();
}

std::uint64_t frames_presented() noexcept
{
    return g_display.presented.load();
}

bool capture_requested() noexcept
{
    const bool asked = g_capture_requested;
    g_capture_requested = false;
    return asked;
}

bool ensure_directory(const std::string &path) noexcept
{
    std::string built;
    for (std::size_t at = 0; at <= path.size(); ++at)
    {
        if (at != path.size() && path[at] != '/')
        {
            built.push_back(path[at]);
            continue;
        }
        if (!built.empty() && mkdir(built.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
        if (at != path.size())
            built.push_back('/');
    }
    return true;
}

/*
 * A minimal PNG writer. zlib does the compression; the rest is the chunk
 * framing. Writing the file from the host rather than shipping raw words means
 * a capture can be read straight back by any tool without a conversion step
 * that could itself be the thing introducing an artefact.
 */
bool write_png(const std::string &path) noexcept
{
    const int w = g_display.width;
    const int h = g_display.height;
    if (w <= 0 || h <= 0 || g_display.linear.empty())
        return false;

    /* One filter byte per row, then RGBA. The scan-out word is ABGR, so its
       bytes in memory are already R, G, B, A. */
    std::vector<unsigned char> raw(static_cast<std::size_t>(h) * (1 + w * 4));
    for (int y = 0; y < h; ++y)
    {
        unsigned char *row = raw.data() + static_cast<std::size_t>(y) * (1 + w * 4);
        row[0] = 0;
        std::memcpy(row + 1, g_display.linear.data() + static_cast<std::size_t>(y) * w,
                    static_cast<std::size_t>(w) * 4);
    }

    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6) !=
        Z_OK)
        return false;
    packed.resize(packed_size);

    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;

    const auto be32 = [](std::uint32_t value, unsigned char *out)
    {
        out[0] = static_cast<unsigned char>(value >> 24);
        out[1] = static_cast<unsigned char>(value >> 16);
        out[2] = static_cast<unsigned char>(value >> 8);
        out[3] = static_cast<unsigned char>(value);
    };
    const auto chunk = [&](const char *type, const unsigned char *data, std::size_t size)
    {
        unsigned char header[4];
        be32(static_cast<std::uint32_t>(size), header);
        (void)std::fwrite(header, 1, 4, file);
        (void)std::fwrite(type, 1, 4, file);
        if (size != 0)
            (void)std::fwrite(data, 1, size, file);
        uLong sum = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
        if (size != 0)
            sum = crc32(sum, data, static_cast<uInt>(size));
        unsigned char tail[4];
        be32(static_cast<std::uint32_t>(sum), tail);
        (void)std::fwrite(tail, 1, 4, file);
    };

    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    (void)std::fwrite(signature, 1, sizeof(signature), file);

    unsigned char header[13];
    be32(static_cast<std::uint32_t>(w), header);
    be32(static_cast<std::uint32_t>(h), header + 4);
    header[8] = 8;  /* bit depth */
    header[9] = 6;  /* truecolour with alpha */
    header[10] = 0; /* deflate */
    header[11] = 0; /* adaptive filtering */
    header[12] = 0; /* no interlace */
    chunk("IHDR", header, sizeof(header));
    chunk("IDAT", packed.data(), packed.size());
    chunk("IEND", nullptr, 0);
    (void)std::fclose(file);
    return true;
}

} // namespace slopfin::host

/* ===================================================================== */
/* The PS5 C ABI, as the console's own sources declare it.                */
/* ===================================================================== */

extern "C"
{

    /* ------------------------------------------------------------- memory */

    std::size_t sceKernelGetDirectMemorySize()
    {
        /* Generous, and only ever compared against what the frames need. */
        return std::size_t{5} * 1024 * 1024 * 1024;
    }

    namespace
    {
    /* Direct memory is faked as ordinary anonymous mappings. The "physical
       address" handed back is the mapping itself, which is all the caller does
       with it. */
    std::mutex g_direct_mutex;
    std::vector<std::pair<std::int64_t, void *>> g_direct;
    } // namespace

    int sceKernelAllocateDirectMemory(std::int64_t /*search_start*/, std::int64_t /*search_end*/,
                                      std::size_t length, std::size_t alignment,
                                      int /*memory_type*/, std::int64_t *physical_address)
    {
        void *memory = nullptr;
        if (posix_memalign(&memory, alignment < sizeof(void *) ? sizeof(void *) : alignment,
                           length) != 0)
            return -1;
        std::memset(memory, 0, length);
        std::lock_guard<std::mutex> guard(g_direct_mutex);
        const auto handle = static_cast<std::int64_t>(g_direct.size() + 1);
        g_direct.emplace_back(handle, memory);
        if (physical_address != nullptr)
            *physical_address = handle;
        return 0;
    }

    int sceKernelMapDirectMemory(void **address, std::size_t /*length*/, int /*protection*/,
                                 int /*flags*/, std::int64_t physical_address,
                                 std::size_t /*alignment*/)
    {
        std::lock_guard<std::mutex> guard(g_direct_mutex);
        for (const auto &entry : g_direct)
            if (entry.first == physical_address)
            {
                *address = entry.second;
                return 0;
            }
        return -1;
    }

    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int /*protection*/,
                                        int /*flags*/, const char * /*name*/)
    {
        void *memory =
            mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
            return -1;
        *address = memory;
        return 0;
    }

    int sceKernelMunmap(void *address, std::size_t length)
    {
        return munmap(address, length);
    }

    int sceKernelAvailableFlexibleMemorySize(std::size_t *size)
    {
        if (size != nullptr)
            *size = std::size_t{1400} * 1024 * 1024;
        return 0;
    }

    /* --------------------------------------------------------------- time */

    int sceKernelGettimeofday(void *value)
    {
        struct HostTime
        {
            std::int64_t seconds;
            std::int64_t microseconds;
        };
        auto *out = static_cast<HostTime *>(value);
        if (g_display.fixed_step)
        {
            /* One sixtieth of a second per presented frame. Everything paced off
               this clock -- the springs, the playback position, the timeouts the
               interface measures -- then runs at exactly console speed whatever
               the host is doing. */
            const std::uint64_t micros = g_display.presented.load() * 16667u;
            out->seconds = static_cast<std::int64_t>(micros / 1000000u);
            out->microseconds = static_cast<std::int64_t>(micros % 1000000u);
            return 0;
        }
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        out->seconds = now.tv_sec;
        out->microseconds = now.tv_nsec / 1000;
        return 0;
    }

    int sceKernelGettimezone(void *value)
    {
        struct HostZone
        {
            int minutes_west;
            int dst;
        };
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        auto *out = static_cast<HostZone *>(value);
        out->minutes_west = static_cast<int>(-local.tm_gmtoff / 60);
        out->dst = local.tm_isdst > 0 ? 1 : 0;
        return 0;
    }

    int sceKernelGetProcessTime()
    {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<int>(now.tv_sec * 1000000 + now.tv_nsec / 1000);
    }

    int sceKernelUsleep(std::uint32_t microseconds)
    {
        return usleep(microseconds);
    }

    /* ------------------------------------------------------------ threads */

    int scePthreadCreate(void **thread, const void * /*attr*/, void *(*entry)(void *),
                         void *argument, const char *name)
    {
        pthread_t created{};
        const int result = pthread_create(&created, nullptr, entry, argument);
        if (result != 0)
            return -1;
        if (name != nullptr)
            (void)pthread_setname_np(created, std::string{name}.substr(0, 15).c_str());
        if (thread != nullptr)
            *thread = reinterpret_cast<void *>(created);
        return 0;
    }

    int scePthreadJoin(void *thread, void **value)
    {
        return pthread_join(reinterpret_cast<pthread_t>(thread), value);
    }

    void *scePthreadSelf()
    {
        return reinterpret_cast<void *>(pthread_self());
    }

    int scePthreadGetaffinity(void * /*thread*/, std::uint64_t *mask)
    {
        if (mask != nullptr)
            *mask = 0x3f;
        return 0;
    }

    int scePthreadSetaffinity(void * /*thread*/, std::uint64_t /*mask*/)
    {
        return 0;
    }

    /* ------------------------------------------------------------ display */

    int sceSystemServiceHideSplashScreen()
    {
        return 0;
    }

    int sceVideoOutOpen(std::int32_t /*user_id*/, std::int32_t /*bus*/, std::int32_t /*index*/,
                        const void * /*param*/)
    {
        return 1;
    }

    int sceVideoOutClose(std::int32_t /*handle*/)
    {
        return 0;
    }

    int sceVideoOutSetFlipRate(std::int32_t /*handle*/, std::int32_t /*rate*/)
    {
        return 0;
    }

    int sceVideoOutGetResolutionStatus(std::int32_t /*handle*/, void *status)
    {
        auto *words = static_cast<std::uint32_t *>(status);
        words[0] = 1920;
        words[1] = 1080;
        words[2] = 1920;
        words[3] = 1080;
        const std::uint64_t refresh = 60;
        std::memcpy(words + 4, &refresh, sizeof(refresh));
        return 0;
    }

    void sceVideoOutSetBufferAttribute2(void * /*attribute*/, std::uint64_t /*pixel_format*/,
                                        std::uint32_t /*tiling*/, std::uint32_t width,
                                        std::uint32_t height, std::uint64_t /*option*/,
                                        std::uint32_t /*dcc*/, std::uint64_t /*clear*/)
    {
        g_display.width = static_cast<int>(width);
        g_display.height = static_cast<int>(height);
        g_display.blocks_across = (width + kTile - 1) / kTile;
        g_display.block_rows = (height + kTile - 1) / kTile;
    }

    int sceVideoOutRegisterBuffers2(std::int32_t /*handle*/, std::int32_t /*set*/,
                                    std::int32_t /*start*/, void *buffers, std::int32_t count,
                                    void * /*attribute*/, std::int32_t /*category*/,
                                    void * /*option*/)
    {
        struct VideoBuffer
        {
            void *data;
            void *metadata;
            void *reserved0;
            void *reserved1;
        };
        /* The HDR formats the console refuses are refused here too, so the host
           exercises the same fallback the console takes. Only 1920x1080 and
           3840x2160 register, as measured on the console. */
        if (!((g_display.width == 1920 && g_display.height == 1080) ||
              (g_display.width == 3840 && g_display.height == 2160)))
            return -1;
        auto *list = static_cast<VideoBuffer *>(buffers);
        g_display.frames.assign(static_cast<std::size_t>(count), nullptr);
        for (std::int32_t i = 0; i < count; ++i)
            g_display.frames[static_cast<std::size_t>(i)] =
                static_cast<std::uint32_t *>(list[i].data);
        return 0;
    }

    int sceVideoOutUnregisterBuffers(std::int32_t /*handle*/, std::int32_t /*set*/)
    {
        return -1;
    }

    int sceVideoOutSubmitChangeBufferAttribute2(std::int32_t /*handle*/, std::int32_t /*set*/,
                                                void * /*attribute*/, void * /*option*/)
    {
        return -1;
    }

    int sceVideoOutSubmitFlip(std::int32_t /*handle*/, std::int32_t buffer_index,
                              std::uint32_t /*mode*/, std::int64_t /*argument*/)
    {
        if (buffer_index < 0 || static_cast<std::size_t>(buffer_index) >= g_display.frames.size())
            return -1;
        untile(g_display.frames[static_cast<std::size_t>(buffer_index)]);
        g_display.presented.fetch_add(1);

        if (g_display.headless || g_display.renderer == nullptr)
            return 0;

        if (g_display.texture == nullptr)
            g_display.texture =
                SDL_CreateTexture(g_display.renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, g_display.width, g_display.height);
        if (g_display.texture == nullptr)
            return 0;
        (void)SDL_UpdateTexture(g_display.texture, nullptr, g_display.linear.data(),
                                g_display.width * 4);
        (void)SDL_RenderClear(g_display.renderer);
        (void)SDL_RenderCopy(g_display.renderer, g_display.texture, nullptr, nullptr);
        SDL_RenderPresent(g_display.renderer);
        return 0;
    }

    int sceVideoOutWaitVblank(std::int32_t /*handle*/)
    {
        return 0;
    }

    /* The flip already completed above, so the queue reports one every time. */
    int sceKernelCreateEqueue(void **queue, const char * /*name*/)
    {
        *queue = reinterpret_cast<void *>(1);
        return 0;
    }

    int sceKernelDeleteEqueue(void * /*queue*/)
    {
        return 0;
    }

    int sceKernelWaitEqueue(void * /*queue*/, void * /*events*/, int /*count*/, int *received,
                            std::uint32_t * /*timeout*/)
    {
        if (received != nullptr)
            *received = 1;
        return 0;
    }

    int sceVideoOutAddFlipEvent(void * /*queue*/, int /*handle*/, void * /*data*/)
    {
        return 0;
    }

    int sceVideoOutDeleteFlipEvent(void * /*queue*/, int /*handle*/)
    {
        return 0;
    }

    /* The interrogation entry points exist so report_display_info links; on a
       host there is nothing behind them to report. */
    int sceVideoOutGetCurrentOutputMode_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetDeviceCapabilityInfo_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetVblankStatus(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetOutputStatus(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetMonitorInfo(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutSysGetCurrentOutputMode(std::int32_t, ...)
    {
        return -1;
    }
    int sceSystemServiceGetHdrToneMapLuminance(void *, ...)
    {
        return -1;
    }
    int sceSystemServiceGetRenderingMode(void *, ...)
    {
        return -1;
    }
    int sceVideoOutGetHdmiMonitorInfo_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetHdmiRawEdid_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetDeviceInfoEx_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetPortStatusInfo_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetVideoOutModeByBusSpecifier_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutGetPortStatusInfoByBusSpecifier_(std::int32_t, ...)
    {
        return -1;
    }
    int sceVideoOutInitializeOutputOptions(void *, ...)
    {
        return -1;
    }
    int sceVideoOutConfigureOptionsInitialize_(void *, ...)
    {
        return -1;
    }

    /* ---------------------------------------------------------------- pad */

    int scePadInit()
    {
        return 0;
    }

    int sceUserServiceInitialize(void * /*params*/)
    {
        return 0;
    }

    int sceUserServiceGetInitialUser(std::int32_t *user_id)
    {
        if (user_id != nullptr)
            *user_id = 1;
        return 0;
    }

    int sceUserServiceGetForegroundUser(std::int32_t *user_id)
    {
        if (user_id != nullptr)
            *user_id = 1;
        return 0;
    }

    int sceUserServiceGetLoginUserIdList(void *list)
    {
        /* Four slots, the first signed in and the rest empty, which is the
           shape the console answers with for a single-profile machine. */
        if (list != nullptr)
        {
            auto *ids = static_cast<std::int32_t *>(list);
            ids[0] = 1;
            ids[1] = ids[2] = ids[3] = -1;
        }
        return 0;
    }

    int scePadSetTriggerEffect(std::int32_t /*handle*/, const void * /*param*/)
    {
        /* A desk has no adaptive triggers. Reporting success keeps the
           preview on the same code path the console takes. */
        return 0;
    }

    int scePadOpen(std::int32_t /*user*/, std::int32_t /*port*/, std::int32_t /*index*/,
                   const void * /*param*/)
    {
        return 1;
    }

    /* The console resolves the trigger-effect entry point through these; a
   workstation has no libScePad, so the lookup simply finds nothing. */
    int sceKernelLoadStartModule(const char * /*name*/, std::size_t /*argc*/, const void * /*argv*/,
                                 std::uint32_t /*flags*/, void * /*option*/, int * /*result*/)
    {
        return -1;
    }

    int sceKernelDlsym(int /*handle*/, const char * /*symbol*/, void **address)
    {
        if (address != nullptr)
            *address = nullptr;
        return -1;
    }

    int scePadRead(std::int32_t /*handle*/, void *data, std::int32_t count)
    {
        if (count < 1)
            return 0;
        auto *sample = static_cast<unsigned char *>(data);
        std::memset(sample, 0, 120);
        std::lock_guard<std::mutex> guard(g_pad_mutex);
        std::memcpy(sample, &g_pad.buttons, sizeof(g_pad.buttons));
        sample[4] = g_pad.left_x;
        sample[5] = g_pad.left_y;
        sample[6] = g_pad.right_x;
        sample[7] = g_pad.right_y;
        sample[8] = g_pad.trigger_l;
        sample[9] = g_pad.trigger_r;
        sample[76] = 1; /* connected */
        return 1;
    }

    /* --------------------------------------------------------------- misc */

    int sceLncUtilKillLocalProcess(const char * /*reason*/)
    {
        g_display.quit = true;
        return 0;
    }

    /* The system keyboard is replaced by SlopFin's own, so the dialog entry
       points only have to fail cleanly. */
    int sceCommonDialogInitialize()
    {
        return 0;
    }
    int sceSysmoduleLoadModule(std::uint16_t)
    {
        return -1;
    }
    int sceImeDialogInit(const void *, const void *)
    {
        return -1;
    }
    int sceImeDialogGetStatus()
    {
        return 0;
    }
    int sceImeDialogGetResult(void *)
    {
        return -1;
    }
    int sceImeDialogTerm()
    {
        return 0;
    }
    int sceImeDialogAbort()
    {
        return 0;
    }

} // extern "C"
