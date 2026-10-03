/*
 * SlopFin - framebuffer display and 2D drawing.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A double-buffered VideoOut surface plus the primitives the UI needs.
 * Colours are 0xAARRGGBB; the framebuffer itself is opaque RGBA8 sRGB.
 */

#ifndef SLOPFIN_GFX_HPP
#define SLOPFIN_GFX_HPP

#include <cstddef>
#include <cstdint>

namespace slopfin::gfx
{

/*
 * The logical design space. Every screen is authored in these coordinates
 * whatever the display actually is, and the primitives scale on the way to the
 * surface. Rendering at the display's own resolution is what keeps text sharp:
 * a 1080p surface on a 4K output is scaled twice before it reaches the panel.
 */
inline constexpr int kWidth = 1920;
inline constexpr int kHeight = 1080;

using Color = std::uint32_t;

constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return 0xff000000u | (static_cast<Color>(r) << 16) | (static_cast<Color>(g) << 8) | b;
}
constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept
{
    return (static_cast<Color>(a) << 24) | (static_cast<Color>(r) << 16) |
           (static_cast<Color>(g) << 8) | b;
}
constexpr Color with_alpha(Color c, std::uint8_t a) noexcept
{
    return (c & 0x00ffffffu) | (static_cast<Color>(a) << 24);
}

/* VideoOut format 0x8000000022000000 uses ABGR words (RGBA bytes).
 * Drawing and decoded artwork use ARGB words; convert only at scan-out.
 * The PS5 SDL backend uses SDL_PIXELFORMAT_ABGR8888 with this same format.
 */
constexpr std::uint32_t scanout_pixel(Color argb) noexcept
{
    return (argb & 0xff00ff00u) | ((argb & 0x00ff0000u) >> 16) | ((argb & 0x000000ffu) << 16);
}

/* Jellyfin's palette, so the app reads as a member of the family. */
namespace palette
{
inline constexpr Color background = rgb(0x10, 0x10, 0x10);
inline constexpr Color surface = rgb(0x20, 0x20, 0x20);
inline constexpr Color control = rgb(0x27, 0x2a, 0x30);
inline constexpr Color control_border = rgb(0x3c, 0x42, 0x4c);
inline constexpr Color focus_surface = rgb(0x18, 0x29, 0x35);
inline constexpr Color focus_border = rgb(0x62, 0xd5, 0xf4);
inline constexpr Color surface_high = rgb(0x2c, 0x2c, 0x2c);
inline constexpr Color accent = rgb(0xaa, 0x5c, 0xc3);
inline constexpr Color accent_alt = rgb(0x00, 0xa4, 0xdc);
inline constexpr Color text = rgb(0xff, 0xff, 0xff);
inline constexpr Color text_dim = rgb(0xb5, 0xba, 0xc2);
inline constexpr Color text_faint = rgb(0x96, 0x9d, 0xa8);
inline constexpr Color danger = rgb(0xcc, 0x44, 0x44);
inline constexpr Color success = rgb(0x4c, 0xaf, 0x50);
} // namespace palette

/* An RGBA8 image in main memory: decoded artwork, or a glyph atlas. */
struct Bitmap
{
    std::uint32_t *pixels = nullptr;
    int width = 0;
    int height = 0;
    bool hdr10 = false;

    [[nodiscard]] bool valid() const noexcept
    {
        return pixels != nullptr && width > 0 && height > 0;
    }
};

/* Opens VideoOut and allocates both frames. Call once. */
bool initialize() noexcept;

/*
 * Every core this process may run on, as a bit mask, or 0 where that cannot be
 * known (the Linux preview). Valid once initialize() has returned. The render
 * thread and the row workers occupy the lowest of them.
 */
std::uint64_t process_cores() noexcept;
/* The console's Settings > Screen and Video > HDR, as last read. */
enum class SystemHdr : std::uint8_t
{
    unknown,
    on,  /* "On When Supported" */
    off, /* "Off" */
};
SystemHdr read_system_hdr() noexcept;
SystemHdr system_hdr() noexcept;
/* True when /data/slopfin-sdr-only forces SDR regardless of the setting. */
bool sdr_only() noexcept;
/* HDR10 buffers are used for an HDR title only while the console's HDR
   setting is on. */
bool hdr_output() noexcept;
std::uint32_t to_hdr_pixel(Color color) noexcept;
void request_hdr_output(bool enabled) noexcept;
bool hdr_request_pending() noexcept;
/* Render-thread acknowledgement of a pending HDR/SDR mode change. This must
   not depend on clearing the frame: playback can have a picture ready before
   the mode request arrives. */
void apply_pending_output_mode() noexcept;

/* The surface actually being drawn into, chosen from the console's output. */
int physical_width() noexcept;
int physical_height() noexcept;

/*
 * Logical to physical, for callers that rasterise their own coverage masks and
 * so must position them in real pixels. Edges are converted rather than
 * positions and sizes, so neighbouring rectangles still meet exactly when the
 * scale is fractional.
 */
int to_physical_x(int x) noexcept;
int to_physical_y(int y) noexcept;
int to_physical_size(int v) noexcept;
/*
 * Moves everything drawn afterwards by a logical offset, until it is set back
 * to zero. Used for page transitions; it does not affect sizes.
 */
void set_origin(int dx, int dy) noexcept;
int to_logical_size(int v) noexcept;

/* The frame currently being drawn into. */
/* Releases the display; call before the process exits. */
void shutdown() noexcept;

std::uint32_t *back_buffer() noexcept;

/*
 * Redirects drawing into a caller's buffer, so something expensive can be
 * composed once and kept. A target is always a whole surface, because every
 * primitive strides by the surface width; surface_words says how big that is.
 * Not nestable.
 */
bool push_target(std::uint32_t *pixels) noexcept;
void pop_target() noexcept;
std::size_t surface_words() noexcept;

/* Copies a whole composed surface over the frame being drawn. */
void blit_surface(const std::uint32_t *pixels) noexcept;
/* Copy only the top `logical_height` rows of a full-stride cached surface. */
void blit_surface_height(const std::uint32_t *pixels, int logical_height) noexcept;

/* Publishes the drawn frame and waits for vblank. */
void present() noexcept;
/* Render thread only: staging pixels are unchanged from the previous present. */
void reuse_frame() noexcept;

void clear(Color color) noexcept;
void fill_rect(int x, int y, int w, int h, Color color) noexcept;
void rounded_rect(int x, int y, int w, int h, int radius, Color color) noexcept;
void stroke_rounded_rect(int x, int y, int w, int h, int radius, int thickness,
                         Color color) noexcept;
/*
 * A ring, or an arc of one that fades along its length. `from` is where the
 * head sits, in turns clockwise from twelve o'clock; `sweep` is how far the
 * tail runs behind it, and a sweep of one draws a plain ring.
 */
void arc(int cx, int cy, int radius, int thickness, float from, float sweep, Color color) noexcept;

/*
 * A soft shadow under a card, built from rings that fade outward. The offset
 * black rectangle this replaces read as a slab poking out from behind the
 * artwork rather than as a shadow.
 */
void drop_shadow(int x, int y, int w, int h, int radius, int spread,
                 std::uint8_t strength) noexcept;
void vertical_gradient(int x, int y, int w, int h, Color top, Color bottom) noexcept;
void horizontal_gradient(int x, int y, int w, int h, Color left, Color right) noexcept;
void rounded_horizontal_gradient(int x, int y, int w, int h, int radius, Color left,
                                 Color right) noexcept;
/* One-pass version of the common media-page scrim: a constant dim plus a
   vertical fade to opaque and a horizontal fade from the left, all using the
   same RGB tone. Visually equivalent to three separate source-over passes but
   touches the framebuffer once. */
void media_scrim(int height, Color tone, std::uint8_t base_alpha, int vertical_start,
                 int horizontal_width, std::uint8_t horizontal_alpha) noexcept;

/*
 * Draws src scaled into the destination box without cropping. Column mapping
 * is computed once per row rather than per pixel, which matters when this runs
 * every frame over a full-screen picture.
 */
void blit_video(const Bitmap &src, int x, int y, int w, int h) noexcept;

/* Draws src scaled to fit the destination box, preserving aspect and cropping. */
void blit_cover(const Bitmap &src, int x, int y, int w, int h, int radius,
                std::uint8_t alpha) noexcept;
void blit_crop(const Bitmap &src, int source_x, int source_y, int source_side, int x, int y,
               int size, int radius, std::uint8_t alpha = 255) noexcept;
void blit(const Bitmap &src, int x, int y, std::uint8_t alpha) noexcept;

/*
 * The whole image scaled into the box, alpha and all. blit_cover crops to
 * fill; this one never crops, so lettering arrives complete.
 */
void blit_scaled(const Bitmap &src, int x, int y, int w, int h) noexcept;

/*
 * Blends a coverage mask that is already positioned and sized in physical
 * pixels. Glyphs and icons are rasterised at the surface's real size, so
 * scaling their masks again here would undo exactly that.
 */
void blend_mask_physical(int x, int y, int w, int h, const unsigned char *coverage,
                         Color color) noexcept;

/* The same, with the colour swept diagonally across the mask. */
void blend_mask_gradient_physical(int x, int y, int w, int h, const unsigned char *coverage,
                                  Color from, Color to) noexcept;

/*
 * Development aid: asks for a dump of the next frame: a crop in physical pixels at its true
 * size, or all zeroes for the whole surface, shrunk by an integer factor.
 * Sharpness can only be judged at 1:1, and the whole surface at 1:1 is 33 MB
 * at 4K. The write happens in present, where the staging buffer and the tiled
 * framebuffer describe the same frame.
 */
bool request_capture(int x, int y, int w, int h, int shrink) noexcept;

/*
 * Development aid: times each drawing primitive at the surface's real size and
 * writes the numbers to a file. The only reliable way to find which one is
 * deciding the frame rate.
 */
void benchmark(const char *path) noexcept;

/* Report the console's current HDMI output mode. */
void report_output_mode() noexcept;

/* The console's output: its resolution, and its refresh rate in Hz when the
 * ABI names one. False when the console will not say. */
bool output_mode(int &width, int &height, double &hz) noexcept;

/* Report the measured refresh rate and what the television says it accepts. */
void report_display_info() noexcept;

/* Clip rectangle applied by every primitive; used for scrolling rows. */
void push_clip(int x, int y, int w, int h) noexcept;
void pop_clip() noexcept;

} // namespace slopfin::gfx

#endif
