/*
 * SlopFin - system on-screen keyboard.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from ProsperoTV's iptv_ime.c, copyright BlackBearReloaded,
 * GPL-3.0-or-later.
 */

#include "ime.hpp"

#include "trace.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace
{
constexpr std::uint16_t kSysmoduleImeDialog = 0x0096;
constexpr std::uint32_t kCommonDialogAlreadyInitialized = 0x80B80002u;

constexpr std::int32_t kImeTypeDefault = 0;
constexpr std::int32_t kImeTypeBasicLatin = 1;
constexpr std::int32_t kEnterLabelDefault = 0;
constexpr std::int32_t kEnterLabelSearch = 2;
constexpr std::uint32_t kImeOptionPassword = 0x00000004u;

/* Dialog status values. */
constexpr int kStatusNone = 0;
constexpr int kStatusRunning = 1;
constexpr int kStatusFinished = 2;

/* Result outcome values. */
constexpr std::int32_t kOutcomeOk = 0;

constexpr std::size_t kMaxCharacters = 255;
constexpr std::size_t kTitleCharacters = 47;
constexpr std::size_t kPlaceholderCharacters = 95;

struct ImeDialogParam
{
    std::int32_t user_id;
    std::int32_t type;
    std::uint64_t supported_languages;
    std::int32_t enter_label;
    std::int32_t input_method;
    void *filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    std::uint16_t *input_text_buffer;
    float pos_x;
    float pos_y;
    std::int32_t horizontal_alignment;
    std::int32_t vertical_alignment;
    const std::uint16_t *placeholder;
    const std::uint16_t *title;
    std::int8_t reserved[16];
};

struct ImeDialogResult
{
    std::int32_t outcome;
    std::int8_t reserved[12];
};

static_assert(sizeof(ImeDialogParam) == 96, "unexpected PS5 IME parameter layout");
static_assert(sizeof(ImeDialogResult) == 16, "unexpected PS5 IME result layout");

extern "C"
{
    int sceCommonDialogInitialize();
    int sceImeDialogAbort();
    int sceImeDialogGetResult(ImeDialogResult *result);
    int sceImeDialogGetStatus();
    int sceImeDialogInit(const ImeDialogParam *param, const void *extended);
    int sceImeDialogTerm();
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceUserServiceGetForegroundUser(std::int32_t *user_id);
}

bool g_ready = false;
bool g_requested = false;
bool g_active = false;
bool g_have_result = false;
bool g_cancelled = false;

std::string g_initial;
std::string g_title;
std::string g_placeholder;
slopfin::ime::Mode g_mode = slopfin::ime::Mode::text;
std::string g_result;

std::array<std::uint16_t, kMaxCharacters + 1> g_text_buffer{};
std::array<std::uint16_t, kTitleCharacters + 1> g_title_buffer{};
std::array<std::uint16_t, kPlaceholderCharacters + 1> g_placeholder_buffer{};

void to_utf16(const std::string &source, std::uint16_t *out, std::size_t capacity) noexcept
{
    std::size_t written = 0;
    const auto *text = reinterpret_cast<const unsigned char *>(source.c_str());
    while (*text != 0 && written + 1 < capacity)
    {
        std::uint32_t codepoint = *text;
        unsigned extra = 0;
        if ((text[0] & 0xe0u) == 0xc0u)
        {
            codepoint = text[0] & 0x1fu;
            extra = 1;
        }
        else if ((text[0] & 0xf0u) == 0xe0u)
        {
            codepoint = text[0] & 0x0fu;
            extra = 2;
        }
        else if ((text[0] & 0xf8u) == 0xf0u)
        {
            codepoint = text[0] & 0x07u;
            extra = 3;
        }
        ++text;
        for (unsigned i = 0; i < extra; ++i)
        {
            if ((*text & 0xc0u) != 0x80u)
                break;
            codepoint = (codepoint << 6) | (*text & 0x3fu);
            ++text;
        }

        if (codepoint >= 0x10000u && written + 2 < capacity)
        {
            codepoint -= 0x10000u;
            out[written++] = static_cast<std::uint16_t>(0xd800u + (codepoint >> 10));
            out[written++] = static_cast<std::uint16_t>(0xdc00u + (codepoint & 0x3ffu));
        }
        else if (codepoint < 0x10000u)
        {
            out[written++] = static_cast<std::uint16_t>(codepoint);
        }
    }
    out[written] = 0;
}

std::string from_utf16(const std::uint16_t *source) noexcept
{
    std::string out;
    for (std::size_t i = 0; source[i] != 0; ++i)
    {
        std::uint32_t codepoint = source[i];
        if (codepoint >= 0xd800u && codepoint <= 0xdbffu && source[i + 1] >= 0xdc00u &&
            source[i + 1] <= 0xdfffu)
        {
            codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (source[++i] - 0xdc00u);
        }
        if (codepoint < 0x80u)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800u)
        {
            out.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
        else if (codepoint < 0x10000u)
        {
            out.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }
    return out;
}

void start_requested() noexcept
{
    std::int32_t user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) < 0)
    {
        g_requested = false;
        g_cancelled = true;
        g_initial.clear();
        g_text_buffer.fill(0);
        return;
    }

    to_utf16(g_initial, g_text_buffer.data(), g_text_buffer.size());
    to_utf16(g_title, g_title_buffer.data(), g_title_buffer.size());
    to_utf16(g_placeholder, g_placeholder_buffer.data(), g_placeholder_buffer.size());

    ImeDialogParam param{};
    param.user_id = user_id;
    param.type = (g_mode == slopfin::ime::Mode::password) ? kImeTypeBasicLatin : kImeTypeDefault;
    param.enter_label =
        (g_mode == slopfin::ime::Mode::search) ? kEnterLabelSearch : kEnterLabelDefault;
    param.option = (g_mode == slopfin::ime::Mode::password) ? kImeOptionPassword : 0u;
    param.max_text_length = static_cast<std::uint32_t>(kMaxCharacters);
    param.input_text_buffer = g_text_buffer.data();
    param.title = g_title_buffer.data();
    param.placeholder = g_placeholder_buffer.data();
    if (g_mode == slopfin::ime::Mode::search)
    {
        // Native dialog coordinates use the standard 1920x1080 UI space.
        // Right/top alignment follows the OpenOrbis IME ABI. The system owns
        // the panel dimensions and clamps the requested position.
        param.pos_x = 1824.0f;
        param.pos_y = 54.0f;
        param.horizontal_alignment = 2;
        param.vertical_alignment = 0;
    }
    else
    {
        param.horizontal_alignment = 1;
        param.vertical_alignment = 1;
    }

    if (sceImeDialogInit(&param, nullptr) < 0)
    {
        g_requested = false;
        g_cancelled = true;
        g_text_buffer.fill(0);
        g_initial.clear();
        slopfin::trace::mark("ime: dialog would not open");
        return;
    }
    g_requested = false;
    g_active = true;
    g_initial.clear();
    slopfin::trace::mark("ime: dialog opened");
}
} // namespace

namespace slopfin::ime
{

bool initialize() noexcept
{
    if (g_ready)
        return true;
    const int dialog = sceCommonDialogInitialize();
    if (dialog < 0 && static_cast<std::uint32_t>(dialog) != kCommonDialogAlreadyInitialized)
    {
        trace::mark("ime: common dialog unavailable");
        return false;
    }
    if (sceSysmoduleLoadModule(kSysmoduleImeDialog) < 0)
    {
        trace::mark("ime: module would not load");
        return false;
    }
    g_ready = true;
    trace::mark("ime: ready");
    return true;
}

void shutdown() noexcept
{
    if (g_active)
    {
        (void)sceImeDialogAbort();
        (void)sceImeDialogTerm();
    }
    g_active = false;
    g_requested = false;
    g_have_result = false;
    g_cancelled = false;
    g_text_buffer.fill(0);
    g_initial.clear();
    g_result.clear();
}

void request(const std::string &initial, const std::string &title, const std::string &placeholder,
             Mode mode) noexcept
{
    if (!g_ready || g_active || g_requested)
        return;
    g_cancelled = false;
    g_have_result = false;
    g_initial = initial;
    g_title = title;
    g_placeholder = placeholder;
    g_mode = mode;
    g_requested = true;
}

void poll() noexcept
{
    if (!g_ready)
        return;
    if (g_requested && !g_active)
    {
        start_requested();
        return;
    }
    if (!g_active)
        return;

    const int status = sceImeDialogGetStatus();
    if (status == kStatusRunning || status == kStatusNone)
        return;
    if (status != kStatusFinished)
    {
        (void)sceImeDialogTerm();
        g_active = false;
        g_cancelled = true;
        g_text_buffer.fill(0);
        g_initial.clear();
        return;
    }

    ImeDialogResult result{};
    const bool ok = sceImeDialogGetResult(&result) >= 0 && result.outcome == kOutcomeOk;
    if (ok)
    {
        g_result = from_utf16(g_text_buffer.data());
        g_have_result = true;
    }
    else
        g_cancelled = true;
    trace::mark(ok ? "ime: confirmed" : "ime: cancelled");
    g_initial.clear();
    /* Wipe the buffer so a typed password does not linger in memory. */
    g_text_buffer.fill(0);
    (void)sceImeDialogTerm();
    g_active = false;
}

bool available() noexcept
{
    return g_ready;
}

bool take_cancelled() noexcept
{
    const bool cancelled = g_cancelled;
    g_cancelled = false;
    return cancelled;
}

bool busy() noexcept
{
    return g_active || g_requested;
}

bool take_result(std::string &out) noexcept
{
    if (!g_have_result)
        return false;
    g_have_result = false;
    out = g_result;
    g_result.clear();
    return true;
}

} // namespace slopfin::ime
