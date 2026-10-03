/*
 * SlopFin - the signed-in user: profile badge, menu, Profile screen.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console offers applications no photo picker -- its dialogs cover the
 * keyboard, save data and messages, not the media gallery -- so the picture is
 * chosen from the files the gallery keeps: captures under
 * /user/av_contents/photo, and JPEG or PNG pictures on a USB drive. The chosen
 * picture is cropped to a square, shrunk to 512 pixels and re-encoded before
 * it is sent, because a 4K screenshot is several megabytes and this process's
 * heap holds about ten; the server scales an avatar down to draw it anyway.
 */

#include "account.hpp"
#include "avatar_crop.hpp"

#include "background.hpp"
#include "bigalloc.hpp"
#include "gfx.hpp"
#include "http.hpp"
#include "icons.hpp"
#include "images.hpp"
#include "ime.hpp"
#include "jellyfin.hpp"
#include "pad.hpp"
#include "text.hpp"
#include "trace.hpp"
#include "ui_common.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef SLOPFIN_HOST
#include <dirent.h>
#endif

#include "../vendor/stb_image.h"

#define STBIW_MALLOC(size) slopfin::bigalloc::allocate(size)
#define STBIW_REALLOC(pointer, size) slopfin::bigalloc::reallocate(pointer, size)
#define STBIW_FREE(pointer) slopfin::bigalloc::release(pointer)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "../vendor/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#ifndef SLOPFIN_HOST
extern "C" int sceKernelGetdents(int fd, char *buffer, int size);
extern "C" int sceKernelGetdirentries(int fd, char *buffer, int size, long *base);
#endif

namespace slopfin::account
{
namespace
{
using text::Weight;
using namespace slopfin::ui;

/* ------------------------------------------------------------ shared state */

struct Photo
{
    std::string path;
    std::string folder; /* the game or drive it came from */
    std::time_t modified = 0;
};

struct Shared
{
    std::mutex mutex;
    bool user_requested = false;
    bool user_loaded = false;
    jellyfin::User user;

    bool busy = false;
    std::string message;
    bool message_is_error = false;
    /* Set by a finished upload, so the picker can close itself. */
    bool picture_saved = false;

    bool scanning = false;
    bool scanned = false;
    std::vector<Photo> photos;
};
Shared g_shared;

jellyfin::User user_snapshot() noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    return g_shared.user;
}

void say(const std::string &message, bool error) noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    g_shared.message = message;
    g_shared.message_is_error = error;
}

void load_user() noexcept
{
    background::run(
        []
        {
            jellyfin::User user;
            std::string error;
            const bool ok = jellyfin::current_user(user, error);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (ok)
            {
                g_shared.user = std::move(user);
                g_shared.user_loaded = true;
            }
            else
            {
                /* Asked again next time something wants it. */
                g_shared.user_requested = false;
                trace::mark("account: " + error);
            }
        });
}

/* ------------------------------------------------------------ render state */

enum class Page : unsigned char
{
    actions,
    picker,
    confirm,
};

enum class Prompt : unsigned char
{
    none,
    current,
    next,
    confirm,
};

enum class Action : unsigned char
{
    change_picture,
    remove_picture,
    change_password,
    sign_out,
};

struct View
{
    bool menu_open = false;
    int menu_index = 0;

    Page page = Page::actions;
    int action_index = 0;
    int photo_index = 0;
    int photo_top_row = 0;
    avatar::Crop crop;
    Prompt prompt = Prompt::none;
    std::string current_password;
    std::string new_password;
};
View g_view;
/* Frames since the menu opened, for its entrance. */
int g_menu_frames = 0;
/* Frames since the menu was dismissed; it fades and lifts rather than
   vanishing between one frame and the next . */
int g_menu_closing = -1;
constexpr int kMenuFadeFrames = 9;

std::vector<Choice> menu_items() noexcept
{
    std::vector<Choice> items{Choice::profile};
    if (is_admin())
        items.push_back(Choice::dashboard);
    items.push_back(Choice::sign_out);
    return items;
}

const char *menu_label(Choice choice) noexcept
{
    switch (choice)
    {
    case Choice::profile:
        return "Profile";
    case Choice::dashboard:
        return "Dashboard";
    case Choice::sign_out:
        return "Sign Out";
    default:
        return "";
    }
}

std::vector<Action> profile_actions(const jellyfin::User &user) noexcept
{
    std::vector<Action> actions{Action::change_picture};
    if (!user.primary_image_tag.empty())
        actions.push_back(Action::remove_picture);
    actions.push_back(Action::change_password);
    actions.push_back(Action::sign_out);
    return actions;
}

const char *action_label(Action action) noexcept
{
    switch (action)
    {
    case Action::change_picture:
        return "Change picture";
    case Action::remove_picture:
        return "Remove picture";
    case Action::change_password:
        return "Change password";
    case Action::sign_out:
        return "Sign out";
    }
    return "";
}

/* ------------------------------------------------------------ drawing bits */

void draw_avatar(const jellyfin::User &user, int x, int y, int size) noexcept
{
    const gfx::Bitmap *picture =
        user.primary_image_tag.empty() || user.id.empty()
            ? nullptr
            : images::acquire(user.id, user.primary_image_tag, images::Kind::user, size);
    if (picture != nullptr)
    {
        gfx::blit_cover(*picture, x, y, size, size, size / 2, 255);
        return;
    }
    gfx::rounded_rect(x, y, size, size, size / 2, gfx::palette::accent);
    const std::string initial = user.name.empty() ? std::string{"?"} : user.name.substr(0, 1);
    const int glyph = size * 11 / 20;
    text::draw(x + (size - text::measure(initial, glyph, Weight::bold)) / 2,
               y + (size - glyph * 13 / 10) / 2, initial, glyph, Weight::bold, gfx::palette::text);
}

void draw_hint_row(int x, int y, const char *cross, const char *circle) noexcept
{
    if (cross != nullptr)
        (void)draw_button_hints(x, y,
                                {{icons::Icon::ps_cross, cross}, {icons::Icon::ps_circle, circle}});
    else
        (void)draw_button_hints(x, y, {{icons::Icon::ps_circle, circle}});
}

std::string short_date(std::time_t when) noexcept
{
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::tm *parts = std::gmtime(&when);
    if (parts == nullptr)
        return {};
    return std::string{kMonths[parts->tm_mon % 12]} + " " + std::to_string(parts->tm_mday) + ", " +
           std::to_string(parts->tm_year + 1900);
}

/* ------------------------------------------------------------ the picker */

bool is_picture(const std::string &name) noexcept
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string extension = name.substr(dot + 1);
    for (char &c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == "jpg" || extension == "jpeg" || extension == "png";
}

void consider_file(const std::string &path, const std::string &folder,
                   std::vector<Photo> &out) noexcept
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || st.st_size > 24 * 1024 * 1024)
        return;
    out.push_back({path, folder, st.st_mtime});
}

#ifndef SLOPFIN_HOST
/*
 * The gallery and USB drives are outside this app's sandbox. elfldr, the
 * console's payload loader, is asked to run assets/slopfin-sandbox.bin, which
 * moves this process's root to the real one. Done once; the change lasts as
 * long as the process.
 */
bool open_sandbox(std::string &why) noexcept
{
    struct stat st = {};
    if (stat("/user/av_contents", &st) == 0)
        return true;
    const int payload = open("/app0/assets/slopfin-sandbox.bin", O_RDONLY);
    if (payload < 0 || fstat(payload, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024)
    {
        if (payload >= 0)
            (void)close(payload);
        why = "The picture picker's helper is missing from this build.";
        return false;
    }
    std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t have = 0;
    while (have < bytes.size())
    {
        const ssize_t got = read(payload, bytes.data() + have, bytes.size() - have);
        if (got <= 0)
            break;
        have += static_cast<std::size_t>(got);
    }
    (void)close(payload);
    const std::string pid = std::to_string(getpid());
    const int out = open("/data/slopfin-sandbox-pid", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out >= 0)
    {
        (void)write(out, pid.data(), pid.size());
        (void)close(out);
    }
    (void)unlink("/data/slopfin-sandbox-result");
    if (have != bytes.size() || !http::send_bytes("127.0.0.1", 9021, bytes))
    {
        why = "Your captures need the console's payload loader (elfldr, port 9021), which did not "
              "answer.";
        return false;
    }
    for (int i = 0; i < 40; ++i)
    {
        if (stat("/user/av_contents", &st) == 0)
            return true;
        timespec pause{0, 100 * 1000 * 1000};
        (void)nanosleep(&pause, nullptr);
    }
    why = "The console would not let SlopFin see your captures.";
    return false;
}
#endif

/* Depth-first, a few levels down: the gallery keeps captures under
   photo/<app>/<title>/<bucket>/. */
void scan(const std::string &path, const std::string &folder, int depth,
          std::vector<Photo> &out) noexcept
{
    if (depth > 5 || out.size() >= 600)
        return;
#ifdef SLOPFIN_HOST
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr)
        return;
    while (const dirent *entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string full = path + "/" + name;
        struct stat st = {};
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            scan(full, depth == 0 ? name : folder, depth + 1, out);
        else if (is_picture(name))
            consider_file(full, folder, out);
    }
    closedir(dir);
#else
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return;
    std::vector<std::pair<std::string, bool>> entries;
    /* Some console filesystems refuse a small directory read (EINVAL), so the
       buffer is the size of one of their blocks. */
    static char buffer[65536];
    int size = 65536;
    for (;;)
    {
        int got = sceKernelGetdents(fd, buffer, size);
        if (got < 0)
        {
            long base = 0;
            got = sceKernelGetdirentries(fd, buffer, size, &base);
        }
        if (got <= 0)
            break;
        /* FreeBSD's struct dirent: fileno u32, reclen u16, type u8, namlen u8, name. */
        for (int at = 0; at + 8 <= got;)
        {
            std::uint16_t length = 0;
            std::memcpy(&length, buffer + at + 4, sizeof(length));
            if (length < 8 || at + length > got)
                break;
            const auto type = static_cast<unsigned char>(buffer[at + 6]);
            const auto name_length = static_cast<unsigned char>(buffer[at + 7]);
            std::string name(buffer + at + 8, std::min<int>(name_length, length - 8));
            at += length;
            if (name.empty() || name == "." || name == "..")
                continue;
            entries.emplace_back(std::move(name), type == 4 /* DT_DIR */);
        }
    }
    (void)close(fd);
    for (const auto &[name, directory] : entries)
    {
        const std::string full = path + "/" + name;
        if (directory)
            scan(full, depth == 0 ? name : folder, depth + 1, out);
        else if (is_picture(name))
            consider_file(full, folder, out);
    }
#endif
}

void start_scan() noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        if (g_shared.scanning)
            return;
        g_shared.scanning = true;
    }
    background::run(
        []
        {
            std::vector<Photo> found;
#ifdef SLOPFIN_HOST
            if (const char *fixture = std::getenv("SLOPFIN_PHOTOS"); fixture != nullptr)
                scan(fixture, "Pictures", 0, found);
            else if (const char *home = std::getenv("HOME"); home != nullptr)
                scan(std::string{home} + "/Pictures", "Pictures", 0, found);
#else
            std::string why;
            if (!open_sandbox(why))
            {
                trace::mark("account: " + why);
                say(why, true);
            }
            /* Captures are filed under the app that took them: the first level is
               the capture app, the second the game. */
            std::vector<Photo> captures;
            scan("/user/av_contents/photo", {}, 0, captures);
            for (Photo &photo : captures)
            {
                const std::size_t game =
                    photo.path.find('/', std::strlen("/user/av_contents/photo/"));
                const std::size_t end =
                    game == std::string::npos ? game : photo.path.find('/', game + 1);
                if (game != std::string::npos && end != std::string::npos)
                    photo.folder = photo.path.substr(game + 1, end - game - 1);
                found.push_back(std::move(photo));
            }
            for (const char *root :
                 {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/ext0", "/mnt/ext1"})
                scan(root, {}, 1, found);
#endif
            std::sort(found.begin(), found.end(),
                      [](const Photo &a, const Photo &b) { return a.modified > b.modified; });
            trace::mark("account: " + std::to_string(found.size()) + " pictures found");
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.photos = std::move(found);
            g_shared.scanning = false;
            g_shared.scanned = true;
        });
}

void append_bytes(void *context, void *data, int size) noexcept
{
    auto *out = static_cast<std::vector<unsigned char> *>(context);
    const auto *bytes = static_cast<const unsigned char *>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/* Export the selected crop; Jellyfin applies the circular display mask. */
bool avatar_jpeg(const std::string &path, const avatar::Crop &crop, std::vector<unsigned char> &out,
                 std::string &error) noexcept
{
    constexpr int kSide = 512;
    const int fd = open(path.c_str(), O_RDONLY);
    struct stat st = {};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 32 * 1024 * 1024)
    {
        if (fd >= 0)
            (void)close(fd);
        error = "That picture could not be read.";
        return false;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    auto *file = static_cast<unsigned char *>(bigalloc::allocate(size));
    std::size_t have = 0;
    while (file != nullptr && have < size)
    {
        const ssize_t got = read(fd, file + have, size - have);
        if (got <= 0)
            break;
        have += static_cast<std::size_t>(got);
    }
    (void)close(fd);
    int width = 0;
    int height = 0;
    int channels = 0;
    const bool valid_image =
        file != nullptr && have == size &&
        stbi_info_from_memory(file, static_cast<int>(size), &width, &height, &channels) != 0 &&
        width > 0 && height > 0 && static_cast<std::uint64_t>(width) * height <= 64000000;
    unsigned char *pixels = valid_image ? stbi_load_from_memory(file, static_cast<int>(size),
                                                                &width, &height, &channels, 3)
                                        : nullptr;
    if (file != nullptr)
        bigalloc::release(file);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (pixels != nullptr)
            stbi_image_free(pixels);
        error = "That picture could not be opened.";
        return false;
    }

    const avatar::Rect area = crop.rect(width, height);
    const int side = area.side;
    const int left = area.x;
    const int top = area.y;
    const int target = std::min(kSide, side);
    auto *square = static_cast<unsigned char *>(
        bigalloc::allocate(static_cast<std::size_t>(target) * target * 3u));
    if (square == nullptr)
    {
        stbi_image_free(pixels);
        error = "Not enough memory to prepare that picture.";
        return false;
    }
    /* Each output pixel averages the block of source pixels it covers. */
    for (int y = 0; y < target; ++y)
    {
        const int y0 = top + y * side / target;
        const int y1 = std::max(y0 + 1, top + (y + 1) * side / target);
        for (int x = 0; x < target; ++x)
        {
            const int x0 = left + x * side / target;
            const int x1 = std::max(x0 + 1, left + (x + 1) * side / target);
            unsigned sum[3] = {};
            unsigned count = 0;
            for (int sy = y0; sy < y1; ++sy)
            {
                const unsigned char *row =
                    pixels + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x0)) *
                                 3u;
                for (int sx = x0; sx < x1; ++sx, row += 3)
                {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    ++count;
                }
            }
            unsigned char *to =
                square + (static_cast<std::size_t>(y) * static_cast<std::size_t>(target) +
                          static_cast<std::size_t>(x)) *
                             3u;
            for (int c = 0; c < 3; ++c)
                to[c] = static_cast<unsigned char>(sum[c] / count);
        }
    }
    stbi_image_free(pixels);

    out.clear();
    out.reserve(160u * 1024u);
    const int written = stbi_write_jpg_to_func(append_bytes, &out, target, target, 3, square, 90);
    bigalloc::release(square);
    if (written == 0 || out.empty())
    {
        error = "That picture could not be converted.";
        return false;
    }
    return true;
}

void save_picture(const std::string &path, const avatar::Crop &crop) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.busy = true;
        g_shared.message = "Saving your picture";
        g_shared.message_is_error = false;
    }
    background::run(
        [path, crop]
        {
            std::vector<unsigned char> jpeg;
            std::string error;
            bool ok = avatar_jpeg(path, crop, jpeg, error);
            if (ok)
                ok = jellyfin::upload_user_image(jpeg, error);
            jellyfin::User user;
            std::string ignored;
            const bool reloaded = ok && jellyfin::current_user(user, ignored);
            trace::mark("account: picture " + std::string{ok ? "saved, " : "failed, "} +
                        std::to_string(jpeg.size()) + " bytes" + (ok ? "" : ": " + error));
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (reloaded)
                g_shared.user = std::move(user);
            g_shared.busy = false;
            g_shared.picture_saved = ok;
            g_shared.message = ok ? "Picture updated" : error;
            g_shared.message_is_error = !ok;
        });
}

void remove_picture() noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.busy = true;
        g_shared.message = "Removing your picture";
        g_shared.message_is_error = false;
    }
    background::run(
        []
        {
            std::string error;
            const bool ok = jellyfin::delete_user_image(error);
            jellyfin::User user;
            std::string ignored;
            const bool reloaded = ok && jellyfin::current_user(user, ignored);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (reloaded)
                g_shared.user = std::move(user);
            g_shared.busy = false;
            g_shared.message = ok ? "Picture removed" : error;
            g_shared.message_is_error = !ok;
        });
}

void submit_password() noexcept
{
    const std::string current = g_view.current_password;
    const std::string next = g_view.new_password;
    g_view.current_password.assign(g_view.current_password.size(), '\0');
    g_view.new_password.assign(g_view.new_password.size(), '\0');
    g_view.current_password.clear();
    g_view.new_password.clear();
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.busy = true;
        g_shared.message = "Changing your password";
        g_shared.message_is_error = false;
    }
    background::run(
        [current, next]
        {
            std::string error;
            const bool ok = jellyfin::change_password(current, next, error);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.busy = false;
            g_shared.message = ok ? "Password changed" : error;
            g_shared.message_is_error = !ok;
        });
}

/* The system keyboard's answer, when one is outstanding. True while it is. */
bool service_prompt() noexcept
{
    if (g_view.prompt == Prompt::none)
        return false;
    std::string entered;
    if (!ime::take_result(entered))
    {
        if (!ime::busy())
            g_view.prompt = Prompt::none; /* cancelled */
        return true;
    }
    switch (g_view.prompt)
    {
    case Prompt::current:
        g_view.current_password = entered;
        g_view.prompt = Prompt::next;
        ime::request("", "New password", "The password you want from now on", ime::Mode::password);
        break;
    case Prompt::next:
        g_view.new_password = entered;
        g_view.prompt = Prompt::confirm;
        ime::request("", "Confirm new password", "The same password again", ime::Mode::password);
        break;
    case Prompt::confirm:
        g_view.prompt = Prompt::none;
        if (entered != g_view.new_password)
        {
            g_view.current_password.clear();
            g_view.new_password.clear();
            say("The new passwords did not match. Nothing was changed.", true);
        }
        else
            submit_password();
        break;
    case Prompt::none:
        break;
    }
    return true;
}

const int kGridColumns = 5;
const int kThumbW = 300;
const int kThumbH = 169;
const int kThumbGap = 28;
const int kThumbRowPitch = kThumbH + 62;
const int kPickerTop = 250;
const int kPickerRows = 3;

int grid_left() noexcept
{
    return (gfx::kWidth - (kGridColumns * kThumbW + (kGridColumns - 1) * kThumbGap)) / 2;
}

} // namespace

/* ------------------------------------------------------------ public */

void ensure_user() noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        if (g_shared.user_requested)
            return;
        g_shared.user_requested = true;
    }
    load_user();
}

void forget() noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    g_shared.user_requested = false;
    g_shared.user_loaded = false;
    g_shared.user = jellyfin::User{};
    g_shared.message.clear();
    g_view.menu_open = false;
}

bool is_admin() noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    return g_shared.user_loaded && g_shared.user.is_admin;
}

namespace
{
/* The height of the top bar's search chip, so the pair share one line. */
const int kBadgeHeight = kTopChipHeight;
const int kBadgeAvatar = 56;
} // namespace

int badge_width() noexcept
{
    const jellyfin::User user = user_snapshot();
    const std::string name = user.name.empty() ? std::string{"Profile"} : user.name;
    return 8 + kBadgeAvatar + 16 + text::measure(name, 26, Weight::medium) + 20 + 28 + 24;
}

void draw_badge(int right, int y) noexcept
{
    const jellyfin::User user = user_snapshot();
    const std::string name = user.name.empty() ? std::string{"Profile"} : user.name;
    const int width = badge_width();
    const int x = right - width;
    const bool lit = g_view.menu_open;
    gfx::rounded_rect(x, y, width, kBadgeHeight, kBadgeHeight / 2,
                      lit ? gfx::palette::focus_surface : gfx::rgba(0x00, 0x00, 0x00, 0x8c));
    gfx::stroke_rounded_rect(x, y, width, kBadgeHeight, kBadgeHeight / 2, 1,
                             lit ? gfx::palette::focus_border : gfx::rgba(0xff, 0xff, 0xff, 0x24));
    const int avatar_x = x + 8;
    const int avatar_y = y + (kBadgeHeight - kBadgeAvatar) / 2;
    draw_avatar(user, avatar_x, avatar_y, kBadgeAvatar);
    /* A thin ring, so the picture reads as a profile and not as artwork. */
    gfx::stroke_rounded_rect(avatar_x - 2, avatar_y - 2, kBadgeAvatar + 4, kBadgeAvatar + 4,
                             kBadgeAvatar / 2 + 2, 2,
                             gfx::rgba(0xff, 0xff, 0xff, lit ? 0xcc : 0x66));
    int cursor = avatar_x + kBadgeAvatar + 16;
    text::draw(cursor, y + (kBadgeHeight - 35) / 2 + 4, name, 26, Weight::medium,
               gfx::palette::text);
    cursor += text::measure(name, 26, Weight::medium) + 20;
    icons::draw(icons::Icon::ps_square, cursor, y + (kBadgeHeight - 28) / 2, 28,
                icons::button::square);
}

bool menu_open() noexcept
{
    return g_view.menu_open;
}

bool menu_visible() noexcept
{
    return g_view.menu_open || (g_menu_closing >= 0 && g_menu_closing < kMenuFadeFrames);
}

void open_menu() noexcept
{
    ensure_user();
    g_view.menu_open = true;
    g_view.menu_index = 0;
    g_menu_frames = 0;
    g_menu_closing = -1;
}

namespace
{
/* Starts the fade, whichever way the menu was dismissed. */
void begin_close() noexcept
{
    g_view.menu_open = false;
    g_menu_closing = 0;
}
} // namespace

Choice handle_menu_input() noexcept
{
    const std::vector<Choice> items = menu_items();
    const int count = static_cast<int>(items.size());
    g_view.menu_index = std::clamp(g_view.menu_index, 0, count - 1);
    if (pad::pressed(pad::Button::circle) || pad::pressed(pad::Button::square))
    {
        begin_close();
        return Choice::closed;
    }
    if (pad::pressed(pad::Button::down) && g_view.menu_index + 1 < count)
        ++g_view.menu_index;
    else if (pad::pressed(pad::Button::up) && g_view.menu_index > 0)
        --g_view.menu_index;
    else if (pad::pressed(pad::Button::cross))
    {
        begin_close();
        return items[static_cast<std::size_t>(g_view.menu_index)];
    }
    return Choice::none;
}

namespace
{
/* Small line icons for the menu, drawn rather than shipped as pictures. */
void draw_menu_icon(Choice choice, int x, int y, int size, gfx::Color colour) noexcept
{
    const int s = size;
    switch (choice)
    {
    case Choice::profile:
    {
        const int head = s * 2 / 5;
        gfx::rounded_rect(x + (s - head) / 2, y + 1, head, head, head / 2, colour);
        const int body_w = s * 4 / 5;
        gfx::rounded_rect(x + (s - body_w) / 2, y + head + 3, body_w, s - head - 3, s / 3, colour);
        break;
    }
    case Choice::dashboard:
    {
        const int cell = (s - 4) / 2;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                gfx::rounded_rect(x + c * (cell + 4), y + r * (cell + 4), cell, cell, 3, colour);
        break;
    }
    case Choice::sign_out:
    {
        const int door_w = s * 11 / 20;
        gfx::stroke_rounded_rect(x, y, door_w, s, 4, 3, colour);
        const int arrow_y = y + s / 2 - 1;
        gfx::fill_rect(x + door_w / 2, arrow_y, s - door_w / 2 - 2, 3, colour);
        for (int k = 0; k < 6; ++k)
        {
            gfx::fill_rect(x + s - 3 - k, arrow_y - k, 3, 3, colour);
            gfx::fill_rect(x + s - 3 - k, arrow_y + k, 3, 3, colour);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

void draw_menu(int right, int top) noexcept
{
    /*
     * Not a panel: the choices drop out of the badge as pills of their own, one
     * after another, with the shared dark fill and cyan focus outline.
     */
    const std::vector<Choice> items = menu_items();
    const bool closing = !g_view.menu_open;
    if (closing)
        ++g_menu_closing;
    else
        ++g_menu_frames;
    /* One number for the whole menu on the way out: the pills leave together,
       lifting back towards the badge, rather than unwinding one by one. */
    const float leaving = closing ? 1.0f - std::clamp(static_cast<float>(g_menu_closing) /
                                                          static_cast<float>(kMenuFadeFrames),
                                                      0.0f, 1.0f)
                                  : 1.0f;
    const float leaving_ease = leaving * leaving; /* quick at first, then gone */
    constexpr int kPillH = 62;
    constexpr int kGap = 10;
    constexpr int kIcon = 24;
    constexpr int kLabel = 25;
    int widest = 0;
    for (Choice choice : items)
        widest = std::max(widest, text::measure(menu_label(choice), kLabel, Weight::medium));
    /* A little narrower than the badge and centred under it, so the column
       reads as hanging from the badge rather than from the screen's edge. */
    const int badge_w = badge_width();
    const int centre = right - badge_w / 2;
    const int content_w = kIcon + 14 + widest;
    const int pill_w = std::max(content_w + 44, badge_w - 24);
    const int x = centre - pill_w / 2;

    /* A light hush over the page, heaviest in the corner the menu comes from. */
    const float appear = std::min(1.0f, static_cast<float>(g_menu_frames) / 10.0f) * leaving_ease;
    gfx::horizontal_gradient(0, 0, gfx::kWidth, gfx::kHeight,
                             gfx::rgba(0, 0, 0, static_cast<std::uint8_t>(0x30 * appear)),
                             gfx::rgba(0, 0, 0, static_cast<std::uint8_t>(0x90 * appear)));

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        /* Each pill follows the one before by a few frames and settles into place. */
        const float t = std::clamp(
            (static_cast<float>(g_menu_frames) - static_cast<float>(i) * 3.0f) / 12.0f, 0.0f, 1.0f);
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        int y = top + 14 + static_cast<int>(i) * (kPillH + kGap) -
                static_cast<int>((1.0f - eased) * 22.0f);
        if (closing)
        {
            /* Back towards the badge, the lower pills travelling furthest, so
               the column collects itself instead of dropping out of sight. */
            y -= static_cast<int>((1.0f - leaving) * (10.0f + static_cast<float>(i) * 8.0f));
            eased *= leaving_ease;
        }
        const auto alpha = [eased](std::uint8_t a) { return static_cast<std::uint8_t>(a * eased); };
        const bool focused = static_cast<int>(i) == g_view.menu_index;
        const gfx::Color fill = focused ? gfx::with_alpha(gfx::palette::focus_surface, alpha(0xf2))
                                        : gfx::rgba(0x14, 0x14, 0x18, alpha(0xe6));
        const gfx::Color ink = focused ? gfx::rgba(0xff, 0xff, 0xff, alpha(0xff))
                                       : gfx::rgba(0xff, 0xff, 0xff, alpha(0xd8));
        if (focused)
            gfx::drop_shadow(x, y, pill_w, kPillH, kPillH / 2, 16, alpha(0x70));
        gfx::rounded_rect(x, y, pill_w, kPillH, kPillH / 2, fill);
        gfx::stroke_rounded_rect(x, y, pill_w, kPillH, kPillH / 2, focused ? 2 : 1,
                                 focused ? gfx::with_alpha(gfx::palette::focus_border, alpha(0xff))
                                         : gfx::rgba(0xff, 0xff, 0xff, alpha(0x22)));
        /* Icon and label as one group in the middle of the pill; the group is as
           wide as the longest label, so the icons still line up. */
        const int group_x = centre - content_w / 2;
        draw_menu_icon(items[i], group_x, y + (kPillH - kIcon) / 2, kIcon, ink);
        text::draw(group_x + kIcon + 14,
                   text::centered_y(y, kPillH, menu_label(items[i]), kLabel, Weight::medium),
                   menu_label(items[i]), kLabel, Weight::medium, ink);
    }
    const int hints_y = top + 14 + static_cast<int>(items.size()) * (kPillH + kGap) + 8;
    const int hints_w = 22 + 9 + text::measure("Select", 20, Weight::regular) + 30 + 22 + 9 +
                        text::measure("Close", 20, Weight::regular);
    if (appear >= 1.0f && !closing)
        (void)draw_button_hints(
            centre - hints_w / 2, hints_y,
            {{icons::Icon::ps_cross, "Select"}, {icons::Icon::ps_circle, "Close"}});
}

void open_profile() noexcept
{
    ensure_user();
    g_view.page = Page::actions;
    g_view.action_index = 0;
    g_view.prompt = Prompt::none;
    say({}, false);
}

Choice handle_profile_input() noexcept
{
    if (service_prompt())
        return Choice::none;

    bool busy = false;
    bool saved = false;
    std::size_t photo_count = 0;
    jellyfin::User user;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        busy = g_shared.busy;
        saved = g_shared.picture_saved;
        g_shared.picture_saved = false;
        photo_count = g_shared.photos.size();
        user = g_shared.user;
    }
    if (saved)
        g_view.page = Page::actions;

    switch (g_view.page)
    {
    case Page::actions:
    {
        const std::vector<Action> actions = profile_actions(user);
        const int count = static_cast<int>(actions.size());
        g_view.action_index = std::clamp(g_view.action_index, 0, count - 1);
        if (pad::pressed(pad::Button::circle))
            return Choice::closed;
        if (pad::pressed(pad::Button::down) && g_view.action_index + 1 < count)
            ++g_view.action_index;
        else if (pad::pressed(pad::Button::up) && g_view.action_index > 0)
            --g_view.action_index;
        else if (pad::pressed(pad::Button::cross) && !busy)
        {
            switch (actions[static_cast<std::size_t>(g_view.action_index)])
            {
            case Action::change_picture:
                g_view.page = Page::picker;
                g_view.photo_index = 0;
                g_view.photo_top_row = 0;
                say({}, false);
                start_scan();
                break;
            case Action::remove_picture:
                remove_picture();
                break;
            case Action::change_password:
                say({}, false);
                if (user.has_password)
                {
                    g_view.prompt = Prompt::current;
                    ime::request("", "Current password", "Your password now", ime::Mode::password);
                }
                else
                {
                    g_view.prompt = Prompt::next;
                    ime::request("", "New password", "The password you want from now on",
                                 ime::Mode::password);
                }
                break;
            case Action::sign_out:
                return Choice::sign_out;
            }
        }
        return Choice::none;
    }
    case Page::picker:
    {
        if (pad::pressed(pad::Button::circle))
        {
            g_view.page = Page::actions;
            return Choice::none;
        }
        if (pad::pressed(pad::Button::triangle))
            start_scan();
        const int count = static_cast<int>(photo_count);
        if (count == 0)
            return Choice::none;
        int index = std::clamp(g_view.photo_index, 0, count - 1);
        if (pad::pressed(pad::Button::right) && index + 1 < count)
            ++index;
        else if (pad::pressed(pad::Button::left) && index > 0)
            --index;
        else if (pad::pressed(pad::Button::down) && index + kGridColumns < count)
            index += kGridColumns;
        else if (pad::pressed(pad::Button::up) && index >= kGridColumns)
            index -= kGridColumns;
        else if (pad::pressed(pad::Button::cross))
        {
            g_view.crop = {};
            g_view.page = Page::confirm;
        }
        g_view.photo_index = index;
        const int row = index / kGridColumns;
        if (row < g_view.photo_top_row)
            g_view.photo_top_row = row;
        else if (row >= g_view.photo_top_row + kPickerRows)
            g_view.photo_top_row = row - kPickerRows + 1;
        return Choice::none;
    }
    case Page::confirm:
    {
        if (busy)
            return Choice::none;
        if (pad::pressed(pad::Button::circle))
        {
            g_view.page = Page::picker;
            say({}, false);
        }
        else if (pad::pressed(pad::Button::cross))
        {
            std::string path;
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                if (g_view.photo_index < static_cast<int>(g_shared.photos.size()))
                    path = g_shared.photos[static_cast<std::size_t>(g_view.photo_index)].path;
            }
            if (!path.empty() && images::acquire(path, "crop", images::Kind::file, 1200) != nullptr)
                save_picture(path, g_view.crop);
        }
        else
        {
            std::string path;
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                if (g_view.photo_index < static_cast<int>(g_shared.photos.size()))
                    path = g_shared.photos[static_cast<std::size_t>(g_view.photo_index)].path;
            }
            const gfx::Bitmap *picture = images::acquire(path, "crop", images::Kind::file, 1200);
            if (picture != nullptr)
            {
                if (pad::pressed(pad::Button::triangle))
                    g_view.crop = {};
                if (pad::pressed(pad::Button::l1))
                    g_view.crop.zoom -= 0.15;
                if (pad::pressed(pad::Button::r1))
                    g_view.crop.zoom += 0.15;
                g_view.crop.zoom += (pad::trigger_right() - pad::trigger_left()) * 0.025;
                g_view.crop.constrain(picture->width, picture->height);
                const double dx = (pad::pressed(pad::Button::right) ? 0.035 : 0.0) -
                                  (pad::pressed(pad::Button::left) ? 0.035 : 0.0);
                const double dy = (pad::pressed(pad::Button::down) ? 0.035 : 0.0) -
                                  (pad::pressed(pad::Button::up) ? 0.035 : 0.0);
                g_view.crop.move(dx, dy, picture->width, picture->height);
            }
        }
        return Choice::none;
    }
    }
    return Choice::none;
}

void draw_profile(const std::string &server_name) noexcept
{
    constexpr int kLeft = 160;
    jellyfin::User user;
    std::string message;
    bool message_is_error = false;
    bool busy = false;
    bool scanning = false;
    bool scanned = false;
    std::vector<Photo> photos;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        user = g_shared.user;
        message = g_shared.message;
        message_is_error = g_shared.message_is_error;
        busy = g_shared.busy;
        scanning = g_shared.scanning;
        scanned = g_shared.scanned;
        if (g_view.page != Page::actions)
            photos = g_shared.photos;
    }
    const gfx::Color message_colour = message_is_error ? gfx::palette::danger
                                      : busy           ? gfx::palette::text_dim
                                                       : gfx::palette::success;

    if (g_view.page == Page::actions)
    {
        text::draw(kLeft, kSafeY + 50, "Profile", 38, Weight::bold, gfx::palette::text);
        draw_avatar(user, kLeft, 230, 300);
        const int x = kLeft + 380;
        text::draw_ellipsized(x, 236, kContentRight - x, user.name.empty() ? "Loading" : user.name,
                              56, Weight::bold, gfx::palette::text);
        text::draw(x, 314, user.is_admin ? "Administrator" : "User", 24, Weight::regular,
                   gfx::palette::text_dim);
        if (!server_name.empty())
            text::draw(x, 352, "Signed in to " + server_name, 22, Weight::regular,
                       gfx::palette::text_faint);

        const std::vector<Action> actions = profile_actions(user);
        const int top = 440;
        focus_pill(x - 28, top + g_view.action_index * 80 - 6, 560, 66, FocusGroup::account);
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const int y = top + static_cast<int>(i) * 80;
            const bool focused = static_cast<int>(i) == g_view.action_index;
            control_label(x, y - 6, 504, 66, action_label(actions[i]), 32, Weight::medium,
                          focused ? gfx::palette::text : gfx::palette::text_dim);
        }
        if (!message.empty())
            text::draw(x, top + static_cast<int>(actions.size()) * 80 + 24, message, 24,
                       Weight::medium, message_colour);
        draw_hint_row(kLeft, gfx::kHeight - 100, "Select", "Back");
        return;
    }

    if (g_view.page == Page::picker)
    {
        text::draw(kLeft, kSafeY + 50, "Choose a picture", 38, Weight::bold, gfx::palette::text);
        const std::string subtitle =
            scanning
                ? std::string{"Looking through your captures and USB drives"}
                : std::to_string(photos.size()) + (photos.size() == 1 ? " picture" : " pictures") +
                      " from your captures and USB drives";
        text::draw(kLeft, kSafeY + 104, subtitle, 22, Weight::regular, gfx::palette::text_dim);
        if (photos.empty())
        {
            if (scanned && !scanning)
                text::draw_wrapped(
                    kLeft, 320, 1200, 3, 40,
                    "No JPEG or PNG pictures were found. Take a screenshot with the Create "
                    "button, or plug in a USB drive that has pictures on it, then press "
                    "Triangle to look again.",
                    26, Weight::regular, gfx::palette::text_dim);
            draw_hint_row(kLeft, gfx::kHeight - 100, nullptr, "Back");
            return;
        }
        const int left = grid_left();
        const int count = static_cast<int>(photos.size());
        const int first = g_view.photo_top_row * kGridColumns;
        const int last = std::min(count, first + kPickerRows * kGridColumns);
        for (int i = first; i < last; ++i)
        {
            const int column = i % kGridColumns;
            const int row = i / kGridColumns - g_view.photo_top_row;
            const int x = left + column * (kThumbW + kThumbGap);
            const int y = kPickerTop + row * kThumbRowPitch;
            if (y > gfx::kHeight - 60)
                break;
            const Photo &photo = photos[static_cast<std::size_t>(i)];
            const gfx::Bitmap *thumb =
                images::acquire(photo.path, "t", images::Kind::file, kThumbH);
            if (thumb != nullptr)
                gfx::blit_cover(*thumb, x, y, kThumbW, kThumbH, 10, 255);
            else
                gfx::rounded_rect(x, y, kThumbW, kThumbH, 10, gfx::palette::surface_high);
            const bool focused = i == g_view.photo_index;
            if (focused)
                gfx::stroke_rounded_rect(x - 4, y - 4, kThumbW + 8, kThumbH + 8, 14, 3,
                                         gfx::palette::text);
            text::draw_ellipsized(x, y + kThumbH + 10, kThumbW, short_date(photo.modified), 19,
                                  focused ? Weight::medium : Weight::regular,
                                  focused ? gfx::palette::text : gfx::palette::text_dim);
            if (!photo.folder.empty())
                text::draw_ellipsized(x, y + kThumbH + 34, kThumbW, photo.folder, 16,
                                      Weight::regular, gfx::palette::text_faint);
        }
        (void)draw_button_hints(kLeft, gfx::kHeight - 70,
                                {{icons::Icon::ps_cross, "Choose"},
                                 {icons::Icon::ps_triangle, "Look again"},
                                 {icons::Icon::ps_circle, "Back"}});
        return;
    }

    text::draw(kLeft, kSafeY + 50, "Adjust your picture", 38, Weight::bold, gfx::palette::text);
    text::draw(kLeft, kSafeY + 104, "Move and zoom until it fits the circle.", 22, Weight::regular,
               gfx::palette::text_dim);
    if (g_view.photo_index < static_cast<int>(photos.size()))
    {
        const Photo &photo = photos[static_cast<std::size_t>(g_view.photo_index)];
        const gfx::Bitmap *picture = images::acquire(photo.path, "crop", images::Kind::file, 1200);
        constexpr int kCircle = 540;
        constexpr int kCircleY = 240;
        gfx::rounded_rect(kLeft, kCircleY, kCircle, kCircle, kCircle / 2,
                          gfx::palette::surface_high);
        if (picture != nullptr)
        {
            const avatar::Rect area = g_view.crop.rect(picture->width, picture->height);
            gfx::blit_crop(*picture, area.x, area.y, area.side, kLeft, kCircleY, kCircle,
                           kCircle / 2);
            gfx::blit_crop(*picture, area.x, area.y, area.side, kLeft + 770, 270, 200, 100);
        }
        else
            text::draw(kLeft + 100, kCircleY + kCircle / 2,
                       images::failed(photo.path, "crop", images::Kind::file)
                           ? "Could not open this picture."
                           : "Loading picture...",
                       22, Weight::regular, gfx::palette::text_dim);
        gfx::stroke_rounded_rect(kLeft - 3, kCircleY - 3, kCircle + 6, kCircle + 6, kCircle / 2 + 3,
                                 2, gfx::palette::accent_alt);
        text::draw(kLeft + 770, 500, "Your picture", 28, Weight::medium, gfx::palette::text);
        const int percent = static_cast<int>(std::lround(g_view.crop.zoom * 100.0));
        text::draw(kLeft + 770, 564, "Zoom  " + std::to_string(percent) + "%", 24, Weight::regular,
                   gfx::palette::text_dim);
        gfx::rounded_rect(kLeft + 770, 615, 360, 6, 3, gfx::palette::surface_high);
        const int filled = std::max(6, static_cast<int>(360 * (g_view.crop.zoom - 1.0) / 5.0));
        gfx::rounded_rect(kLeft + 770, 615, filled, 6, 3, gfx::palette::accent_alt);
        (void)draw_button_hints(kLeft + 770, 674, {{icons::Icon::ps_triangle, "Reset crop"}});
        text::draw_ellipsized(kLeft, 824, 1200,
                              short_date(photo.modified) +
                                  (photo.folder.empty() ? "" : "   " + photo.folder),
                              22, Weight::regular, gfx::palette::text_faint);
    }
    if (!message.empty())
        text::draw(kLeft, 880, message, 24, Weight::medium, message_colour);
    if (!busy)
        (void)draw_button_hints(kLeft, gfx::kHeight - 70,
                                {{icons::Icon::ps_cross, "Use picture"},
                                 {icons::Icon::ps_cross, "Move", "D-pad"},
                                 {icons::Icon::ps_cross, "Zoom", "L1 / R1"},
                                 {icons::Icon::ps_circle, "Back"}});
}

} // namespace slopfin::account
