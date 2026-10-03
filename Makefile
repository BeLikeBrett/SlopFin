# ps5-native-app-boilerplate - Linux/WSL build entry points.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

SHELL := /bin/bash
.DEFAULT_GOAL := app

-include .env

APP_DEFINITIONS ?=
APP_INCLUDE_PATHS ?=
APP_STATIC_ARCHIVES ?=
# The build script's path filter rejects '+', so the SDK's C++ runtime archives
# are copied into the dependency cache under names without it. Refresh with tools/refresh-sdk-archives.sh.
APP_STATIC_ARCHIVES += .deps/native/cxx/libcxx.a .deps/native/cxx/libcxxabi.a .deps/native/cxx/libunwind.a
APP_RUNTIME_MODULES ?=
SOFTWARE_AUDIO ?= 0
ifeq ($(SOFTWARE_AUDIO),1)
APP_DEFINITIONS += SLOPFIN_SOFTWARE_AUDIO
APP_INCLUDE_PATHS += .deps/audio/install/include
APP_STATIC_ARCHIVES += .deps/audio/install/lib/libavcodec.a .deps/audio/install/lib/libswresample.a .deps/audio/install/lib/libavutil.a
endif
PACBREW_PACKAGES ?=
PACBREW_INCLUDE_PATHS ?=
PACBREW_STATIC_ARCHIVES ?=
PS5_HOST ?=
FTP_PORT ?= 2121
DEPLOY_FORMAT ?= folder
PS5_FTP_USER ?= anonymous
PS5_FTP_PASSWORD ?= codex
DEPLOY_DRY_RUN ?= 0
TITLE_ID ?=
APP_NAME ?=
APP_CATEGORY ?= game
CONTENT_SUFFIX ?=
HOST_CXX ?= clang++
HOST_CC ?= clang
HOST_TEST_CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections
HOST_TEST_LDFLAGS ?= -Wl,--gc-sections
GTEST_ARGS ?=
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_RUNTIME_MODULES
export PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES
export PS5_HOST FTP_PORT DEPLOY_FORMAT PS5_FTP_USER PS5_FTP_PASSWORD DEPLOY_DRY_RUN
export TITLE_ID APP_NAME APP_CATEGORY CONTENT_SUFFIX

# ---------------------------------------------------------------- host build
# The Linux preview: the same renderer, screens, input handling and Jellyfin
# client, on a backend that implements the PS5 C ABI over SDL2 and POSIX.
# See docs/HOST_BUILD.md. player.cpp and audio.cpp stay on the console; host/
# supplies a stand-in so the playback overlay is drivable here.
HOST_BIN := build/host/slopfin
HOST_SRC := \
	host/host_main.cpp host/host_platform.cpp host/host_player.cpp \
	src/app.cpp src/gfx.cpp src/text.cpp src/icons.cpp src/images.cpp \
	src/ime.cpp src/http.cpp src/web_transport.cpp src/jellyfin.cpp src/json.cpp src/pad.cpp \
	src/bigalloc.cpp src/config.cpp src/subtitles.cpp src/telemetry.cpp \
	src/reporter.cpp src/trace.cpp src/crash.cpp src/warm.cpp src/ui_common.cpp src/account.cpp src/background.cpp src/dashboard.cpp src/update.cpp src/update_package.cpp
HOST_CXXFLAGS ?= -std=c++20 -O2 -g -DSLOPFIN_HOST -Wall -Wextra \
	-Wno-unused-parameter -pthread $(shell pkg-config --cflags sdl2)
HOST_LDFLAGS ?= -pthread $(shell pkg-config --libs sdl2 libcurl) -lz

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)

.PHONY: all app build init doctor test test-unit test-integration libc deps pacbrew pacbrew-list assets-check format format-check tidy lint check ffpkg ffpfsc packages deploy undeploy clean distclean help host host-run

all: app
build: app

init:
	@printf '%s\n' '==> [init] Configuring the application identity in sce_sys/param.json'
	@bash tools/init-project.sh sce_sys/param.json

doctor:
	@printf '%s\n' '==> [doctor] Checking the Linux/WSL host without changing it'
	@bash tools/doctor.sh

host: $(HOST_BIN)

$(HOST_BIN): $(HOST_SRC) $(wildcard src/*.hpp host/*.hpp)
	@printf '%s\n' '==> [host] Building the Linux preview'
	@mkdir -p -- $(@D)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $(HOST_SRC) $(HOST_LDFLAGS) -o $@

host-run: $(HOST_BIN)
	@./$(HOST_BIN) $(HOST_ARGS)

test: test-unit test-integration test-server-network

.PHONY: test-playback test-updates
test-playback: test-updates
	@mkdir -p build/tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -DSLOPFIN_HOST -Isrc tests/test_config.cpp src/config.cpp src/json.cpp -o build/tests/config_tests
	@build/tests/config_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_icons.cpp src/icons.cpp -o build/tests/icon_tests
	@build/tests/icon_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_audio_buffer_gate.cpp -o build/tests/audio_buffer_gate_tests
	@build/tests/audio_buffer_gate_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Wno-unused-function -pthread -Isrc tests/test_images_retry.cpp $(HOST_TEST_LDFLAGS) -o build/tests/images_retry_tests
	@build/tests/images_retry_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_adts_frames.cpp -o build/tests/adts_frames_tests
	@build/tests/adts_frames_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -pthread -Isrc tests/test_packet_queue.cpp -o build/tests/packet_queue_tests
	@build/tests/packet_queue_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_hevc_base_layer.cpp -o build/tests/hevc_base_layer_tests
	@build/tests/hevc_base_layer_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -pthread -Isrc tests/test_audio_backpressure.cpp $(HOST_TEST_LDFLAGS) -o build/tests/audio_backpressure_tests
	@build/tests/audio_backpressure_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_media_bitrate.cpp -o build/tests/media_bitrate_tests
	@build/tests/media_bitrate_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_telemetry.cpp -o build/tests/telemetry_tests
	@build/tests/telemetry_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_negotiation.cpp src/json.cpp $(HOST_TEST_LDFLAGS) -o build/tests/negotiation_tests
	@build/tests/negotiation_tests
	@mkdir -p build/tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_playback.cpp -o build/tests/playback_tests
	@build/tests/playback_tests
	$(HOST_CXX) -std=c++20 -O2 -pthread -ffunction-sections -fdata-sections tests/test_audio_lifetime.cpp $(HOST_TEST_LDFLAGS) -o build/tests/audio_lifetime_tests
	@build/tests/audio_lifetime_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_subtitles.cpp src/subtitles.cpp -o build/tests/subtitle_tests
	@build/tests/subtitle_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_tsdemux.cpp src/tsdemux.cpp -o build/tests/tsdemux_tests
	@build/tests/tsdemux_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_ac3.cpp -o build/tests/ac3_tests
	@build/tests/ac3_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_iec61937.cpp -o build/tests/iec61937_tests
	@build/tests/iec61937_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_video_sps.cpp -o build/tests/video_sps_tests
	@build/tests/video_sps_tests
	@mkdir -p build/tests/crash
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -fno-omit-frame-pointer -DSLOPFIN_HOST -Isrc \
		tests/test_crash_report.cpp src/crash.cpp -o build/tests/crash_report_tests
	@cd build/tests/crash && ../crash_report_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_hdr.cpp -o build/tests/hdr_tests
	@build/tests/hdr_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_video_scale.cpp -o build/tests/video_scale_tests
	@build/tests/video_scale_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_playback_url.cpp -o build/tests/playback_url_tests
	@build/tests/playback_url_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_cadence.cpp -o build/tests/cadence_tests
	@build/tests/cadence_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_motion.cpp -o build/tests/motion_tests
	@build/tests/motion_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_ime.cpp -o build/tests/ime_tests
	@build/tests/ime_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_server_address.cpp -o build/tests/server_address_tests
	@build/tests/server_address_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_search_sections.cpp -o build/tests/search_sections_tests
	@build/tests/search_sections_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -pthread -Isrc tests/test_web_native.cpp src/web_transport.cpp -o build/tests/web_native_tests
	@build/tests/web_native_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_seek.cpp -o build/tests/seek_tests
	@build/tests/seek_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_home_rows.cpp -o build/tests/home_rows_tests
	@build/tests/home_rows_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_stream_clock.cpp -o build/tests/stream_clock_tests
	@build/tests/stream_clock_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_subtitle_timing.cpp src/subtitles.cpp -o build/tests/subtitle_timing_tests
	@build/tests/subtitle_timing_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_details_sheet.cpp -o build/tests/details_sheet_tests
	@build/tests/details_sheet_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_delivery_summary.cpp -o build/tests/delivery_summary_tests
	@build/tests/delivery_summary_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_autoplay.cpp -o build/tests/autoplay_tests
	@build/tests/autoplay_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_subtitle_style.cpp -o build/tests/subtitle_style_tests
	@build/tests/subtitle_style_tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_intro_metadata.cpp src/json.cpp -o build/tests/intro_metadata_tests
	@build/tests/intro_metadata_tests

	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_avatar_crop.cpp -o build/tests/avatar_crop_tests
	@build/tests/avatar_crop_tests

	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Wno-unused-function -pthread -DSLOPFIN_HOST $(shell pkg-config --cflags sdl2) -Isrc tests/test_rounded_render.cpp src/gfx.cpp src/bigalloc.cpp host/host_platform.cpp $(shell pkg-config --libs sdl2) -lz $(HOST_TEST_LDFLAGS) -o build/tests/rounded_render_tests
	@build/tests/rounded_render_tests

	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Wno-unused-function -pthread -DSLOPFIN_HOST $(shell pkg-config --cflags sdl2) -Isrc tests/test_avatar_editor.cpp src/gfx.cpp src/text.cpp src/icons.cpp src/images.cpp src/pad.cpp src/bigalloc.cpp src/config.cpp src/json.cpp src/ui_common.cpp src/background.cpp src/ime.cpp src/jellyfin.cpp src/http.cpp src/web_transport.cpp src/trace.cpp src/crash.cpp host/host_platform.cpp $(shell pkg-config --libs sdl2 libcurl) -lz $(HOST_TEST_LDFLAGS) -o build/tests/avatar_editor_tests
	@SLOPFIN_DATA="$(CURDIR)/build/tests/avatar-config" SLOPFIN_ASSETS="$(CURDIR)/assets" build/tests/avatar_editor_tests

# Compatibility alias for contributors using the standard unit-test target.
test-unit: test-playback

test-updates:
	@mkdir -p build/tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -Isrc tests/test_update_package.cpp src/update_package.cpp src/json.cpp -o build/tests/update_package_tests
	@build/tests/update_package_tests
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wpedantic -Werror -Isrc tests/test_update_transaction.c -o build/tests/update_transaction_tests

test-integration: test-updates
	@printf '%s\n' '==> [test-integration] Running host tooling integration tests'
	@python3 -m unittest discover -s tests -p 'test_*.py' -v

deps:
	@printf '%s\n' '==> [deps] Fetching declared native dependencies'
	@bash tools/setup-native-dependencies.sh
	@bash tools/setup-pacbrew-dependencies.sh --environment

pacbrew:
	@printf '%s\n' '==> [pacbrew] Fetching the pinned prebuilt ports sysroot'
	@bash tools/setup-pacbrew-dependencies.sh --all

pacbrew-list:
	@printf '%s\n' '==> [pacbrew] Listing available pkg-config modules'
	@bash tools/setup-pacbrew-dependencies.sh --list

assets-check:
	@printf '%s\n' '==> [assets] Validating icon, backgrounds, and selection audio'
	@bash tools/validate-assets.sh

sdk-archives:
	@bash tools/setup-native-dependencies.sh >/dev/null
	@bash tools/refresh-sdk-archives.sh

libc:
	@printf '%s\n' '==> [libc] Rebuilding and verifying the clean-room runtime'
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the missing or outdated runtime'
	@bash tools/rebuild-libc.sh

# Raw payload data uses .bin so native packagers do not sign it as a module.
SANDBOX_ELF := assets/slopfin-sandbox.bin
$(SANDBOX_ELF): payloads/sandbox/main.c
	@printf '%s\n' '==> [payload] Building the picture picker sandbox payload'
	@PS5_PAYLOAD_SDK=$(CURDIR)/.deps/native/ps5-payload-sdk $(MAKE) -s -C payloads/sandbox slopfin-sandbox.elf
	@cp payloads/sandbox/slopfin-sandbox.elf $@
	@rm -f assets/slopfin-sandbox.elf

UPDATE_ELF := assets/slopfin-update.bin
$(UPDATE_ELF): payloads/updater/main.c $(wildcard src/update/*.h)
	@printf '%s\n' '==> [payload] Building the app update helper'
	@PS5_PAYLOAD_SDK=$(CURDIR)/.deps/native/ps5-payload-sdk $(MAKE) -s -C payloads/updater slopfin-update.elf
	@cp payloads/updater/slopfin-update.elf $@

app: sdk-archives $(RUNTIME) $(SANDBOX_ELF) $(UPDATE_ELF)
	@printf '%s\n' '==> [app] Compiling, linking, signing, and assembling the app folder'
	@bash tools/build.sh Folder

ffpkg: sdk-archives $(RUNTIME) $(SANDBOX_ELF) $(UPDATE_ELF)
	@printf '%s\n' '==> [ffpkg] Building the app folder and UFS2 image'
	@bash tools/build.sh Ffpkg

ffpfsc: sdk-archives $(RUNTIME) $(SANDBOX_ELF) $(UPDATE_ELF)
	@printf '%s\n' '==> [ffpfsc] Building the app folder and compressed image'
	@bash tools/build.sh Ffpfsc

packages: sdk-archives $(RUNTIME) $(SANDBOX_ELF) $(UPDATE_ELF)
	@printf '%s\n' '==> [packages] Building the app folder and both package formats'
	@bash tools/build.sh All

deploy:
	@printf '%s\n' '==> [deploy] Building and publishing the selected app output over FTP'
	@bash tools/deploy.sh

undeploy:
	@printf '%s\n' '==> [undeploy] Removing staged development files for this title over FTP'
	@bash tools/deploy.sh undeploy

format:
	@printf '%s\n' '==> [format] Formatting C and C++ sources'
	@bash tools/run_clang_format.sh

format-check:
	@printf '%s\n' '==> [format] Checking C and C++ formatting'
	@bash tools/run_clang_format.sh --check

tidy:
	@printf '%s\n' '==> [tidy] Running Clang static analysis'
	@bash tools/run_clang_tidy.sh

lint:
	@printf '%s\n' '==> [lint] Running source, metadata, and shell checks'
	@bash tools/lint.sh

check: lint test host app

clean:
	@printf '%s\n' '==> [clean] Removing generated build outputs'
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME)

distclean: clean
	@printf '%s\n' '==> [distclean] Removing downloaded dependency caches'
	@rm -rf -- .deps

help:
	@printf '%s\n' \
	  'make                 Generate libc.prx and build the SlopFin folder' \
	  'make init TITLE_ID=PPSA12345 APP_NAME="My App"  Configure app identity' \
	  'make doctor          Check required and optional Linux/WSL tools' \
	  'make test            Run app model, tooling and network tests' \
	  'make test-unit       Run playback and app model tests' \
	  'make test-integration  Run host tooling integration tests' \
	  'make deps            Fetch native dependencies into .deps/' \
	  'make pacbrew         Fetch the pinned PacBrew ports sysroot' \
	  'make pacbrew-list    List PacBrew pkg-config module names' \
	  'make assets-check    Validate the current presentation assets' \
	  'make libc            Force a deterministic runtime/libc.prx rebuild' \
	  'make format          Apply the shared Clang formatting policy' \
	  'make format-check    Check formatting without modifying files' \
	  'make tidy            Run the shared Clang static-analysis policy' \
	  'make lint            Run format, tidy, metadata, and shell checks' \
	  'make check           Run lint and build the app' \
	  'make ffpkg           Build the folder and UFS2 .ffpkg image' \
	  'make fpkg            Build and verify a native PS5 debug .pkg' \
	  'make ffpfsc          Build the folder and compressed .ffpfsc image' \
	  'make packages        Build folder, .ffpkg, and .ffpfsc outputs' \
	  'make deploy PS5_HOST=<address>  Build and FTP-deploy the app folder' \
	  'make undeploy PS5_HOST=<address>  Remove this title from /data/homebrew' \
	  'Build variables:     APP_DEFINITIONS, APP_INCLUDE_PATHS, APP_STATIC_ARCHIVES, APP_RUNTIME_MODULES' \
	  'PacBrew variables:   PACBREW_PACKAGES, PACBREW_INCLUDE_PATHS, PACBREW_STATIC_ARCHIVES' \
	  'Deploy variables:    FTP_PORT=2121, DEPLOY_FORMAT=folder|ffpfsc|ffpkg, DEPLOY_DRY_RUN=0|1' \
	  'Local defaults:      Copy .env.example to the ignored .env file' \
	  'make host            Build the Linux preview into build/host/slopfin' \
	  'make host-run        Build and run it (HOST_ARGS=... to pass flags)' \
	  'make clean           Remove build/, dist/, and generated libc.prx' \
	  'make distclean       Also remove the ignored .deps/ cache'

.PHONY: sdk-archives test-server-network
test-server-network:
	@mkdir -p build/tests
	$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -DSLOPFIN_HOST -Isrc tests/test_web_transport.cpp src/http.cpp src/web_transport.cpp src/json.cpp $(shell pkg-config --libs libcurl) -pthread -o build/tests/web_transport_tests
	@python3 tools/test-server-network.py build/tests/web_transport_tests

.PHONY: fpkg
fpkg: app
	@bash tools/build-native-pkg.sh
