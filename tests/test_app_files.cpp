/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_files.hpp"
#include "update/files.h"
#include <cassert>
#include <filesystem>

int main()
{
    namespace fs = std::filesystem;
    char temporary[] = "/tmp/slopfin-app-files-XXXXXX";
    const char *folder = mkdtemp(temporary);
    assert(folder);
    const fs::path base(folder), app = base / "app0", original = base / "original";
    fs::create_directories(app / "assets");
    assert(sf_update_write((app / "assets/ca.pem").c_str(), "original", 8) == 0);
    slopfin::app_files::Directory directory;
    assert(!directory.capture((base / "missing").c_str()));
    assert(directory.capture(app.c_str()));
    fs::rename(app, original);
    fs::create_directories(app / "assets");
    assert(sf_update_write((app / "assets/ca.pem").c_str(), "decoy", 5) == 0);
    assert(directory.capture(app.c_str()));
    int fd = directory.open("assets/ca.pem");
    assert(fd >= 0);
    char text[16] = {};
    assert(read(fd, text, sizeof(text)) == 8 && std::string_view(text) == "original");
    assert(close(fd) == 0);
    fs::create_symlink(app / "assets/ca.pem", original / "link");
    fs::create_directory_symlink(app / "assets", original / "directory-link");
    for (const char *invalid :
         {"", "/etc/passwd", "../app0/assets/ca.pem", "assets/../link", "assets//ca.pem",
          "assets/./ca.pem", "assets/", "link", "directory-link/ca.pem", "missing"})
        assert(directory.open(invalid) < 0);
    assert(directory.open(std::string_view("assets/ca.pem\0suffix", 20)) < 0);
    assert(directory.open(std::string(201, 'x')) < 0);
    fd = directory.open("assets/ca.pem");
    assert(fd >= 0);
    char hex[65];
    assert(sf_update_hash_fd(fd, hex) == 0);
    assert(fcntl(fd, F_GETFD) >= 0); // Hashing must leave the descriptor owned by its caller.
    assert(close(fd) == 0);
    // An installed package refuses relative opens: the same file is reached by
    // name while the captured path still names the directory...
    {
        const fs::path packaged = base / "packaged", sandboxes = base / "sandbox";
        fs::create_directories(packaged / "assets");
        assert(sf_update_write((packaged / "assets/ca.pem").c_str(), "packaged", 8) == 0);
        slopfin::app_files::Directory package;
        package.refuse_relative = true;
        assert(package.capture(packaged.c_str(), sandboxes.c_str()));
        fd = package.open("assets/ca.pem");
        assert(fd >= 0 && read(fd, text, sizeof(text)) == 8 &&
               std::string_view(text, 8) == "packaged");
        assert(close(fd) == 0);
        assert(package.open("assets/missing") < 0);
        assert(package.open("../original/assets/ca.pem") < 0);
        // ...and after a root change, under the sandbox mounts by identity only:
        // a decoy at the old path or a same-named sandbox elsewhere is ignored.
        fs::create_directories(sandboxes / "PPSA00002_000/app0/assets");
        assert(sf_update_write((sandboxes / "PPSA00002_000/app0/assets/ca.pem").c_str(), "other",
                               5) == 0);
        fs::create_directories(sandboxes / "PPSA00001_000");
        fs::rename(packaged, sandboxes / "PPSA00001_000/app0");
        fs::create_directories(packaged / "assets");
        assert(sf_update_write((packaged / "assets/ca.pem").c_str(), "decoy", 5) == 0);
        fd = package.open("assets/ca.pem");
        char found[16] = {};
        assert(fd >= 0 && read(fd, found, sizeof(found)) == 8 &&
               std::string_view(found, 8) == "packaged");
        assert(close(fd) == 0);
        // A file kept before the move is still read whole, and its hash given,
        // with no way to open or duplicate anything afterwards.
        {
            slopfin::app_files::Directory kept;
            const fs::path early = base / "early";
            fs::create_directories(early / "assets");
            assert(sf_update_write((early / "assets/ca.pem").c_str(), "kept", 4) == 0);
            assert(sf_update_write((early / "eboot.bin").c_str(), "code", 4) == 0);
            assert(kept.capture(early.c_str()));
            kept.refuse_relative = true;
            assert(kept.keep("assets/ca.pem") && !kept.keep("assets/absent"));
            assert(kept.keep("eboot.bin", 0)); // hash only
            assert(kept.kept_count() == 2);
            char expected[65];
            assert(sf_update_hash_file((early / "eboot.bin").c_str(), expected) == 0);
            fs::remove_all(early);
            for (int pass = 0; pass < 2; ++pass)
            {
                std::string bytes;
                assert(kept.kept_bytes("assets/ca.pem", bytes) && bytes == "kept");
            }
            std::string none;
            assert(!kept.kept_bytes("eboot.bin", none) && !kept.kept_bytes("assets/other", none));
            char hex[65];
            assert(kept.kept_hash("eboot.bin", hex) && std::string_view(hex) == expected);
            assert(!kept.kept_hash("assets/other", hex));
        }
        // The package mount behind a sandbox is accepted too.
        fs::create_directories(sandboxes / "pfsmnt");
        fs::rename(sandboxes / "PPSA00001_000/app0", sandboxes / "pfsmnt/PPSA00001-app0");
        fd = package.open("assets/ca.pem");
        assert(fd >= 0 && close(fd) == 0);
        fs::remove_all(sandboxes / "pfsmnt/PPSA00001-app0");
        assert(package.open("assets/ca.pem") < 0);
    }
    fs::remove_all(base);
}
