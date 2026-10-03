/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/account.cpp"
#include "../host/host_platform.hpp"
#include <cassert>
#include <chrono>
#include <thread>
int main()
{
    using namespace slopfin;
    host::Options options;
    options.headless = true;
    assert(host::open_display(options) && gfx::initialize() && text::initialize() &&
           pad::initialize());
    gfx::clear(gfx::rgb(220, 30, 30));
    gfx::fill_rect(960, 0, 960, 1080, gfx::rgb(20, 40, 220));
    gfx::present();
    const std::string path = "build/tests/avatar-source.png";
    assert(host::write_png(path));
    images::start();
    account::g_shared.photos.push_back({path, "Fixture", 0});
    account::g_view.page = account::Page::confirm;
    const gfx::Bitmap *picture = nullptr;
    for (int i = 0; i < 100 && !picture; ++i)
    {
        picture = images::acquire(path, "crop", images::Kind::file, 1200);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(picture);
    auto press = [](pad::Button button)
    {
        pad::inject(button);
        pad::poll();
        account::handle_profile_input();
        pad::poll();
    };
    for (int i = 0; i < 5; ++i)
        press(pad::Button::r1);
    for (int i = 0; i < 60; ++i)
        press(pad::Button::right);
    assert(account::g_view.crop.zoom > 1.5);
    std::vector<unsigned char> jpeg;
    std::string error;
    assert(account::avatar_jpeg(path, account::g_view.crop, jpeg, error));
    int w = 0, h = 0, channels = 0;
    auto *decoded =
        stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &channels, 3);
    assert(decoded && w == 512 && h == 512);
    for (int x : {0, 256, 511})
    {
        const auto *pixel = decoded + (256 * w + x) * 3;
        assert(pixel[0] < 40 && pixel[2] > 200);
    }
    stbi_image_free(decoded);
    gfx::clear(gfx::palette::background);
    account::draw_profile("Fixture");
    gfx::present();
    assert(host::write_png("build/tests/avatar-editor.png"));
    press(pad::Button::triangle);
    assert(account::g_view.crop.zoom == 1.0 && account::g_view.crop.x == 0.5);
    press(pad::Button::circle);
    assert(account::g_view.page == account::Page::picker);
    gfx::shutdown();
    host::close_display();
    std::_Exit(0);
}
