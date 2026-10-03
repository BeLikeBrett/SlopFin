/*
 * SlopFin - controller input.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pad.hpp"

#include "trace.hpp"

#include <cstddef>

#include <array>
#include <cstring>
#include <string>

extern "C"
{
    int scePadInit();
    int scePadOpen(std::int32_t user_id, std::int32_t port_type, std::int32_t index,
                   const void *param);
    int scePadRead(std::int32_t handle, void *data, std::int32_t count);
    int sceUserServiceInitialize(void *params);
    int sceUserServiceGetInitialUser(std::int32_t *user_id);
    int sceUserServiceGetForegroundUser(std::int32_t *user_id);
    int sceUserServiceGetLoginUserIdList(void *list);
    int sceKernelGetProcessTime();
}

namespace
{
/*
 * ScePadData layout: the button mask is at offset 0, the left stick at 4 and 5,
 * and byte 76 is zero while the pad is not delivering real input. A sample is
 * 120 bytes, not the 96 a PS4 layout would suggest; reading into anything
 * smaller corrupts whatever follows it.
 */
constexpr std::size_t kPadSampleBytes = 120;
constexpr std::size_t kPadSampleCapacity = 64;

/* Set while the system is intercepting input, for example over the shell UI. */
constexpr std::uint32_t kButtonIntercepted = 0x80000000u;
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

/* Stick deflection past this counts as a direction. */
constexpr int kStickThreshold = 60;

/*
 * The right stick's dead zone, as a fraction of full travel. A DualSense at
 * rest does not report exactly centre, and a list that creeps while nobody is
 * touching the pad is worse than one that does not scroll at all.
 */
constexpr float kScrollDeadZone = 0.22f;

/* Frames before a held direction starts repeating, and the repeat interval. */
constexpr int kRepeatDelayFrames = 24;
constexpr int kRepeatIntervalFrames = 5;
/* Trigger repeats run faster than directional cursor repeats. */
constexpr int kTriggerDelayFrames = 10;
constexpr int kTriggerIntervalFrames = 2;

constexpr auto kCount = static_cast<std::size_t>(slopfin::pad::Button::count);

std::int32_t g_handle = -1;

/* Use the foreground user; initial user ID 0 is the system pseudo-user and cannot own a pad. */
constexpr std::int32_t kMaxLoginUsers = 4;

std::int32_t resolve_user(const char **how) noexcept
{
    std::int32_t user_id = 0;
    if (sceUserServiceGetForegroundUser(&user_id) >= 0 && user_id > 0)
    {
        if (how != nullptr)
            *how = "foreground";
        return user_id;
    }
    user_id = 0;
    if (sceUserServiceGetInitialUser(&user_id) >= 0 && user_id > 0)
    {
        if (how != nullptr)
            *how = "initial";
        return user_id;
    }
    std::int32_t list[kMaxLoginUsers] = {0, 0, 0, 0};
    if (sceUserServiceGetLoginUserIdList(list) >= 0)
        for (const std::int32_t candidate : list)
            if (candidate > 0)
            {
                if (how != nullptr)
                    *how = "login list";
                return candidate;
            }
    if (how != nullptr)
        *how = "none";
    return 0;
}
int g_reopen_countdown = 0;
int g_reopen_reports = 0;
std::array<bool, kCount> g_now{};
std::array<bool, kCount> g_before{};
std::array<int, kCount> g_hold_frames{};
std::array<bool, kCount> g_fired{};
std::array<bool, kCount> g_injected{};
/* Frames an injected button is still to be held down for. */
std::array<int, kCount> g_injected_hold{};
float g_scroll_axis = 0.0f;
float g_trigger_l = 0.0f;
float g_trigger_r = 0.0f;

bool fast_repeat(slopfin::pad::Button button) noexcept
{
    return button == slopfin::pad::Button::l2 || button == slopfin::pad::Button::r2;
}

bool repeats(slopfin::pad::Button button) noexcept
{
    /* The triggers repeat too: holding one scrubs through a title. */
    return button == slopfin::pad::Button::up || button == slopfin::pad::Button::down ||
           button == slopfin::pad::Button::left || button == slopfin::pad::Button::right ||
           button == slopfin::pad::Button::l2 || button == slopfin::pad::Button::r2 ||
           button == slopfin::pad::Button::l1 || button == slopfin::pad::Button::r1;
}
} // namespace

namespace
{
/* Public libScePad trigger-effect ABI. Keep the packed union size assertion. */
constexpr std::uint8_t kTriggerMaskL2 = 0x01;
constexpr std::uint8_t kTriggerMaskR2 = 0x02;
constexpr std::uint32_t kTriggerModeOff = 0;
constexpr std::uint32_t kTriggerModeWeapon = 2;

struct TriggerCommand
{
    std::uint32_t mode;
    std::uint8_t padding[4];
    /* The widest member of the command union, so every mode fits. */
    std::uint8_t data[48];
};

struct TriggerParam
{
    std::uint8_t trigger_mask;
    std::uint8_t padding[7];
    TriggerCommand command[2];
};

static_assert(sizeof(TriggerCommand) == 56, "unexpected trigger command layout");
static_assert(sizeof(TriggerParam) == 120, "unexpected trigger parameter layout");

/* Weapon: where over the travel (0..9) the resistance starts and stops, and
   how hard it pushes back (0..8). */
struct WeaponFeel
{
    std::uint8_t start_position;
    std::uint8_t end_position;
    std::uint8_t strength;
};

int g_trigger_result = -1;
bool g_trigger_tried = false;
/* Byte 76 of a sample: a handle can be open with nothing on the other end. */
bool g_pad_present = false;
slopfin::pad::TriggerFeel g_wanted_feel = slopfin::pad::TriggerFeel::none;
} // namespace

/* Import trigger effects directly; weak imports and the tested dlsym handle did not resolve this
 * symbol. */
extern "C"
{
    int scePadSetTriggerEffect(std::int32_t handle, const void *param);
}

namespace slopfin::pad
{

void set_trigger_feel(TriggerFeel feel) noexcept
{
    g_wanted_feel = feel;
    if (g_handle < 0)
        return;

    TriggerParam param{};
    param.trigger_mask = kTriggerMaskL2 | kTriggerMaskR2;
    for (TriggerCommand &command : param.command)
    {
        if (feel == TriggerFeel::none)
        {
            command.mode = kTriggerModeOff;
            continue;
        }
        command.mode = kTriggerModeWeapon;
        /* Apply resistance near mid-travel while retaining free travel on either side. */
        const WeaponFeel weapon{2, 7, 6};
        std::memcpy(command.data, &weapon, sizeof(weapon));
    }
    g_trigger_result = scePadSetTriggerEffect(g_handle, &param);
    /*
     * Only an answer from a controller that is actually on means anything. An
     * open handle with nothing attached refuses this call, and latching that
     * refusal would turn "the pad was asleep when playback started" into "this
     * console does not have adaptive triggers" for the rest of the session.
     * poll() re-applies the wanted feel when a pad turns up.
     */
    if (!g_trigger_tried && g_pad_present)
    {
        slopfin::trace::mark("pad: trigger effect -> " + std::to_string(g_trigger_result) +
                             " (pad present)");
        g_trigger_tried = true;
    }
    else if (!g_pad_present)
    {
        slopfin::trace::mark("pad: trigger effect -> " + std::to_string(g_trigger_result) +
                             " (no pad attached, not conclusive)");
    }
}

bool trigger_effects_available() noexcept
{
    /* Until it has been asked for once, assume yes: the symbol is exported, so
       the only thing that can say otherwise is the console refusing a real
       call. */
    return !g_trigger_tried || g_trigger_result == 0;
}

int last_trigger_result() noexcept
{
    return g_trigger_result;
}

bool initialize() noexcept
{
    if (sceUserServiceInitialize(nullptr) < 0)
        return false;
    const char *how = "none";
    const std::int32_t user_id = resolve_user(&how);
    if (scePadInit() < 0)
        return false;
    g_handle = scePadOpen(user_id, 0, 0, nullptr);
    slopfin::trace::mark(std::string{"pad: user "} + std::to_string(user_id) + " (" + how +
                         "), handle " + std::to_string(g_handle));

    /*
     * A pad that is not awake yet is not a pad that will never be awake.
     * Opening it once at start-up meant launching while the controller was
     * asleep left the console unable to take input for the whole session, with
     * nothing on screen saying so -- and picking the controller up afterwards
     * changed nothing. poll() retries.
     */
    return true;
}

void inject(Button button) noexcept
{
    g_injected[static_cast<std::size_t>(button)] = true;
}

void inject_hold(Button button, int frames) noexcept
{
    g_injected[static_cast<std::size_t>(button)] = true;
    g_injected_hold[static_cast<std::size_t>(button)] = frames > 0 ? frames : 0;
}

void poll() noexcept
{
    g_before = g_now;
    g_now.fill(false);
    /*
     * Injected presses are handled below whether or not a pad is attached, so
     * this only skips reading one. Returning here instead meant that with no
     * controller connected the remote test tooling drove nothing at all, and
     * a feature that worked looked broken.
     */
    if (g_handle < 0)
    {
        g_scroll_axis = 0.0f;
        g_trigger_l = 0.0f;
        g_trigger_r = 0.0f;
        /* About once a second, which is quick enough that picking the pad up
           feels immediate and rare enough to cost nothing. */
        if (--g_reopen_countdown <= 0)
        {
            g_reopen_countdown = 60;
            /*
             * The user is asked for again each time, not remembered from
             * start-up. Who is signed in can change while the app is running,
             * and a pad opened against a user who has since gone is refused
             * for as long as the stale id is kept.
             */
            const char *how = "none";
            const std::int32_t user_id = resolve_user(&how);
            const std::int32_t opened = scePadOpen(user_id, 0, 0, nullptr);
            if (opened >= 0)
            {
                g_handle = opened;
                slopfin::trace::mark("pad: controller connected");
                if (g_wanted_feel != TriggerFeel::none)
                    set_trigger_feel(g_wanted_feel);
            }
            else if (g_reopen_reports < 3)
            {
                ++g_reopen_reports;
                slopfin::trace::mark("pad: open refused, user " + std::to_string(user_id) + " (" +
                                     how + ") -> " + std::to_string(opened));
            }
        }
    }
    else
    {
        /* Static, not stack: 64 samples of 120 bytes is far too much for a frame. */
        static std::array<std::array<unsigned char, kPadSampleBytes>, kPadSampleCapacity> samples{};
        int count =
            scePadRead(g_handle, samples.data(), static_cast<std::int32_t>(kPadSampleCapacity));
        if (count > static_cast<int>(kPadSampleCapacity))
            count = static_cast<int>(kPadSampleCapacity);
        if (count <= 0)
        {
            /* No fresh sample: keep the previous state so holds do not stutter,
               the stick included -- zeroing it here made a held stick scroll in
               bursts. */
            g_now = g_before;
        }
        else
        {
            /* The newest sample is the one that matters for edge detection. */
            const unsigned char *sample = samples[count - 1].data();
            std::uint32_t buttons = 0;
            std::memcpy(&buttons, sample, sizeof(buttons));
            const bool present = sample[76] != 0;
            if (!present || (buttons & kButtonIntercepted) != 0)
                buttons = 0;
            if (present && !g_pad_present)
            {
                /* A pad that has just woken up has no effect loaded on it. */
                g_pad_present = true;
                if (g_wanted_feel != slopfin::pad::TriggerFeel::none)
                    slopfin::pad::set_trigger_feel(g_wanted_feel);
            }
            g_pad_present = present;
            const auto left_x = static_cast<int>(sample[4]);
            const auto left_y = static_cast<int>(sample[5]);
            /* ScePadData puts the right stick immediately after the left one. */
            const auto right_y = static_cast<int>(sample[7]);
            /* ScePadData: the analogue triggers follow the two sticks. */
            g_trigger_l = static_cast<float>(sample[8]) / 255.0f;
            g_trigger_r = static_cast<float>(sample[9]) / 255.0f;
            const float deflection = static_cast<float>(right_y - 128) / 127.0f;
            const float magnitude = deflection < 0.0f ? -deflection : deflection;
            /* Rescaled past the dead zone, so the first movement that counts is a
               slow one rather than a jump to a fifth of full speed. */
            g_scroll_axis = magnitude <= kScrollDeadZone
                                ? 0.0f
                                : (deflection < 0.0f ? -1.0f : 1.0f) *
                                      (magnitude - kScrollDeadZone) / (1.0f - kScrollDeadZone);

            const auto set = [&](Button button, bool value)
            { g_now[static_cast<std::size_t>(button)] = value; };
            set(Button::up, (buttons & kBtnUp) != 0 || left_y < 128 - kStickThreshold);
            set(Button::down, (buttons & kBtnDown) != 0 || left_y > 128 + kStickThreshold);
            set(Button::left, (buttons & kBtnLeft) != 0 || left_x < 128 - kStickThreshold);
            set(Button::right, (buttons & kBtnRight) != 0 || left_x > 128 + kStickThreshold);
            set(Button::cross, (buttons & kBtnCross) != 0);
            set(Button::circle, (buttons & kBtnCircle) != 0);
            set(Button::square, (buttons & kBtnSquare) != 0);
            set(Button::triangle, (buttons & kBtnTriangle) != 0);
            set(Button::l1, (buttons & kBtnL1) != 0);
            set(Button::r1, (buttons & kBtnR1) != 0);
            set(Button::options, (buttons & kBtnOptions) != 0);
            set(Button::l2, (buttons & kBtnL2) != 0);
            set(Button::r2, (buttons & kBtnR2) != 0);
            set(Button::l3, (buttons & kBtnL3) != 0);
            set(Button::r3, (buttons & kBtnR3) != 0);
            set(Button::touchpad, (buttons & kBtnTouchpad) != 0);
        }
    }

    for (std::size_t i = 0; i < kCount; ++i)
    {
        const auto button = static_cast<Button>(i);
        g_fired[i] = false;
        if (!g_now[i])
        {
            g_hold_frames[i] = 0;
            continue;
        }
        if (!g_before[i])
        {
            g_fired[i] = true;
            g_hold_frames[i] = 0;
            continue;
        }
        ++g_hold_frames[i];
        if (!repeats(button))
            continue;
        const int delay = fast_repeat(button) ? kTriggerDelayFrames : kRepeatDelayFrames;
        const int interval = fast_repeat(button) ? kTriggerIntervalFrames : kRepeatIntervalFrames;
        if (g_hold_frames[i] >= delay && (g_hold_frames[i] - delay) % interval == 0)
            g_fired[i] = true;
    }

    /*
     * An injected press now behaves exactly like a real one, for one whole
     * frame. Clearing it on the first read instead meant a handler that asks
     * about the same button twice saw it only the first time: testing
     * "left or right" and then asking which it was reported neither, so an
     * injected right arrived as a left.
     */
    for (std::size_t i = 0; i < kCount; ++i)
    {
        if (g_injected[i])
        {
            g_fired[i] = true;
            g_injected[i] = false;
        }
        /* A held injection keeps the button down, so held() answers yes for
           as long as it was asked for, exactly as a finger would. */
        if (g_injected_hold[i] > 0)
        {
            --g_injected_hold[i];
            g_now[i] = true;
        }
    }
}

bool pressed(Button button) noexcept
{
    return g_fired[static_cast<std::size_t>(button)];
}

bool held(Button button) noexcept
{
    return g_now[static_cast<std::size_t>(button)];
}

bool any_pressed() noexcept
{
    for (bool fired : g_fired)
        if (fired)
            return true;
    return false;
}

float scroll_axis() noexcept
{
    return g_scroll_axis;
}

float trigger_left() noexcept
{
    return g_trigger_l;
}

float trigger_right() noexcept
{
    return g_trigger_r;
}

} // namespace slopfin::pad
