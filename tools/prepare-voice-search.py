#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the small PS5 runtime adaptations to a disposable Whisper source copy.

The verified upstream archive and extracted source stay untouched. The app uses
only the statically registered CPU backend: do not scan the filesystem for DLLs,
spawn a debugger on abort, or depend on unavailable C++ file-stream symbols.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def replace(path, old, new, count=1):
    text = path.read_text(encoding="utf-8")
    if text.count(old) != count:
        raise SystemExit(f"Unexpected pinned source in {path}: {old[:65]!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


whisper = root / "src/whisper.cpp"
# Both the speech and optional VAD loader use precisely the same read callbacks.
text = whisper.read_text(encoding="utf-8")
# The bundled libc intentionally does not implement FILE reads. Keep an owned
# descriptor on each loader call's stack; every failed/short/interrupted read
# returns its actual byte count, and all exits close the descriptor exactly once.
model_file = r"""
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct slopfin_whisper_model_file {
    int fd = -1;
    off_t size = 0;
    bool at_end = false;

    explicit slopfin_whisper_model_file(const char * path) {
        do { fd = open(path, O_RDONLY); } while (fd < 0 && errno == EINTR);
        if (fd < 0) return;
        struct stat info;
        int result;
        do { result = fstat(fd, &info); } while (result < 0 && errno == EINTR);
        if (result < 0 || info.st_size < 0) { close_file(); return; }
        size = info.st_size;
    }
    ~slopfin_whisper_model_file() { close_file(); }
    slopfin_whisper_model_file(const slopfin_whisper_model_file &) = delete;
    slopfin_whisper_model_file & operator=(const slopfin_whisper_model_file &) = delete;

    size_t read_bytes(void * output, size_t requested) {
        size_t done = 0;
        while (done < requested && fd >= 0) {
            ssize_t received = read(fd, static_cast<unsigned char *>(output) + done, requested - done);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) { at_end = true; break; }
            done += static_cast<size_t>(received);
        }
        return done;
    }
    bool eof() const {
        if (fd < 0 || at_end) return true;
        off_t position;
        do { position = lseek(fd, 0, SEEK_CUR); } while (position < 0 && errno == EINTR);
        return position < 0 || position >= size;
    }
    void close_file() {
        if (fd >= 0) { close(fd); fd = -1; }
    }
};

"""
first_loader = text.index("struct whisper_context * whisper_init_from_file_with_params_no_state(")
text = text[:first_loader] + model_file + text[first_loader:]
for start in ("struct whisper_context * whisper_init_from_file_with_params_no_state(",
              "struct whisper_vad_context * whisper_vad_init_from_file_with_params("):
    begin = text.index(start)
    end = text.index("\nstruct ", begin + len(start))
    block = text[begin:end]
    opening = block.index("#ifdef _MSC_VER")
    ending = block.index("#endif", opening) + len("#endif")
    block = block[:opening] + '    slopfin_whisper_model_file fin(path_model);' + block[ending:]
    block = block.replace("if (!fin)", "if (fin.fd < 0)")
    block = block.replace("std::ifstream * fin = (std::ifstream*)ctx;\n        fin->read((char *)output, read_size);\n        return read_size;",
                          "return static_cast<slopfin_whisper_model_file *>(ctx)->read_bytes(output, read_size);")
    block = block.replace("std::ifstream * fin = (std::ifstream*)ctx;\n        return fin->eof();",
                          "return static_cast<slopfin_whisper_model_file *>(ctx)->eof();")
    block = block.replace("std::ifstream * fin = (std::ifstream*)ctx;\n        fin->close();",
                          "static_cast<slopfin_whisper_model_file *>(ctx)->close_file();")
    if "ifstream" in block:
        raise SystemExit("Pinned model-loader adaptation was incomplete")
    text = text[:begin] + block + text[end:]
# Search audio and its derived spectrogram never go to disk, even in debug mode.
start = text.index('    // Dump log_mel_spectrogram\n')
end = text.index('\n    return true;', start)
text = text[:start] + "    (void) debug; // SlopFin keeps microphone-derived data in memory only.\n" + text[end:]
whisper.write_text(text, encoding="utf-8")

registry = root / "ggml/src/ggml-backend-reg.cpp"
text = registry.read_text(encoding="utf-8").replace('#include "ggml-backend-dl.h"\n', '').replace('#include <filesystem>\n', '')
start = text.index("namespace fs = std::filesystem;")
end = text.index("struct ggml_backend_reg_entry", start)
text = text[:start] + text[end:]
text = text.replace("    dl_handle_ptr handle;\n", "")
start = text.index("    ~ggml_backend_registry() {")
end = text.index("    void register_backend(", start)
text = text[:start] + text[end:]
text = text.replace("void register_backend(ggml_backend_reg_t reg, dl_handle_ptr handle = nullptr)",
                    "void register_backend(ggml_backend_reg_t reg)")
text = text.replace("backends.push_back({ reg, std::move(handle) });", "backends.push_back({ reg });")
start = text.index("    ggml_backend_reg_t load_backend(")
end = text.index("    void unload_backend(", start)
text = text[:start] + text[end:]
start = text.index("// Dynamic loading\n")
text = text[:start] + """// This build has one statically registered CPU backend and no plugin loader.
ggml_backend_reg_t ggml_backend_load(const char *) { return nullptr; }
void ggml_backend_unload(ggml_backend_reg_t) {}
void ggml_backend_load_all() { (void) get_reg(); }
void ggml_backend_load_all_from_path(const char *) { (void) get_reg(); }
"""
registry.write_text(text, encoding="utf-8")
# Do not even compile the now-unused dlopen adapter into the archive.
replace(root / "ggml/src/CMakeLists.txt", "ggml-backend-dl.cpp", "")
# PS5 cannot fork/exec gdb. The normal log/abort callback remains intact.
replace(root / "ggml/src/ggml.c", "#if defined(__linux__) || \\\n", "#if 0 /* PS5: no debugger subprocess support */\n")

# Tensor/model/compute buffers need flexible memory rather than the small libc
# process heap. The app owns these two C hooks and pairs allocation/release.
ggml = root / "ggml/src/ggml.c"
text = ggml.read_text(encoding="utf-8")
start = text.index("void * ggml_aligned_malloc(size_t size) {")
end = text.index("inline static void * ggml_malloc(size_t size)", start)
text = text[:start] + """extern void * slopfin_voice_allocate(size_t size);
extern void slopfin_voice_free(void * pointer);

void * ggml_aligned_malloc(size_t size) {
    return size ? slopfin_voice_allocate(size) : NULL;
}

void ggml_aligned_free(void * pointer, size_t size) {
    GGML_UNUSED(size);
    slopfin_voice_free(pointer);
}

""" + text[end:]
ggml.write_text(text, encoding="utf-8")
