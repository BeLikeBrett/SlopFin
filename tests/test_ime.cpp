/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - system keyboard lifecycle regression tests.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <cassert>
#include <cstdio>
#include "../src/ime.cpp"

namespace
{
int dialog_status = 1;
int init_result = 0;
int outcome = 0;
int term_calls = 0;
int abort_calls = 0;
ImeDialogParam captured{};
} // namespace
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
extern "C"
{
    int sceCommonDialogInitialize()
    {
        return 0;
    }
    int sceSysmoduleLoadModule(std::uint16_t)
    {
        return 0;
    }
    int sceUserServiceGetForegroundUser(std::int32_t *user)
    {
        *user = 7;
        return 0;
    }
    int sceImeDialogInit(const ImeDialogParam *param, const void *)
    {
        captured = *param;
        return init_result;
    }
    int sceImeDialogGetStatus()
    {
        return dialog_status;
    }
    int sceImeDialogGetResult(ImeDialogResult *result)
    {
        result->outcome = outcome;
        return 0;
    }
    int sceImeDialogTerm()
    {
        ++term_calls;
        return 0;
    }
    int sceImeDialogAbort()
    {
        ++abort_calls;
        return 0;
    }
}
int main()
{
    using namespace slopfin;
    assert(ime::initialize() && ime::available());
    ime::request("The Office", "Search", "Series or movie", ime::Mode::search);
    assert(ime::busy());
    ime::poll();
    assert(captured.user_id == 7 && captured.enter_label == 2 && captured.option == 0);
    assert(captured.horizontal_alignment == 2 && captured.vertical_alignment == 0);
    assert(captured.pos_x == 1824.0f && captured.pos_y == 54.0f);
    assert(from_utf16(captured.input_text_buffer) == "The Office");
    dialog_status = 2;
    ime::poll();
    std::string value;
    assert(!ime::busy() && ime::take_result(value) && value == "The Office");
    assert(!ime::take_result(value) && !ime::take_cancelled());
    assert(captured.input_text_buffer[0] == 0);

    dialog_status = 1;
    ime::request("secret", "Password", "", ime::Mode::password);
    ime::poll();
    assert(captured.type == 1 && captured.option == 4);
    assert(captured.horizontal_alignment == 1 && captured.vertical_alignment == 1);
    outcome = 1;
    dialog_status = 2;
    ime::poll();
    assert(!ime::take_result(value) && ime::take_cancelled());
    assert(!ime::take_cancelled() && captured.input_text_buffer[0] == 0);

    init_result = -1;
    ime::request("secret", "Password", "", ime::Mode::password);
    ime::poll();
    assert(!ime::busy() && ime::take_cancelled() && captured.input_text_buffer[0] == 0);
    assert(g_initial.empty());

    // A failed opening must not strand the next request; non-ASCII roundtrips.
    init_result = 0;
    outcome = 0;
    dialog_status = 1;
    ime::request("Amélie 🎬", "Search", "", ime::Mode::search);
    ime::poll();
    assert(from_utf16(captured.input_text_buffer) == "Amélie 🎬");
    dialog_status = 2;
    ime::poll();
    assert(ime::take_result(value) && value == "Amélie 🎬" && term_calls == 3);
    dialog_status = 1;
    ime::request("secret", "Password", "", ime::Mode::password);
    ime::poll();
    assert(ime::busy());
    ime::shutdown();
    assert(!ime::busy() && !ime::take_result(value) && !ime::take_cancelled());
    assert(abort_calls == 1 && term_calls == 4 && captured.input_text_buffer[0] == 0);
    std::puts("IME: search, password, cancel, failed-open retry, Unicode and shutdown passed");
}
