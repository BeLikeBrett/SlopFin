/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_BITMAP_SUBTITLES_HPP
#define SLOPFIN_BITMAP_SUBTITLES_HPP
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slopfin::bitmap_subtitles
{
struct Rect
{
    int x = 0, y = 0, width = 0, height = 0;
    std::shared_ptr<std::uint32_t> pixels; /* straight-alpha ARGB, flexible memory */
};
struct Frame
{
    double start = 0.0, end = 0.0;
    int width = 0, height = 0; /* authored canvas, independent of video resolution */
    std::vector<Rect> rects;   /* an empty frame explicitly clears the screen */
    std::size_t bytes = 0;
};
bool available() noexcept;
/* One transport writer, one render reader. Reset only after joining the writer. */
void reset() noexcept;
void enable(bool value) noexcept;
bool append(std::shared_ptr<Frame> frame) noexcept;
std::shared_ptr<const Frame> current(double seconds) noexcept;

class Decoder
{
    [[maybe_unused]] void *context_ = nullptr;
    [[maybe_unused]] int compression_ = -1;
    void *unpacked_ = nullptr;
    std::vector<std::uint8_t> prefix_;

  public:
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder();
    bool open(const std::string &codec, const std::vector<std::uint8_t> &extra, int width,
              int height, int compression = -1,
              const std::vector<std::uint8_t> &settings = {}) noexcept;
    bool decode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                std::int64_t duration) noexcept; /* time units: 90 kHz */
};
} // namespace slopfin::bitmap_subtitles
#endif
