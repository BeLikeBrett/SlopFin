/*
 * SlopFin - link-only facade for the audio decoder module.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The public payload SDK ships no stub for libSceAudiodec. These definitions
 * exist only so the linker and the module writer can attribute the imports;
 * the console supplies the real implementation.
 */

int sceAudiodecInitLibrary(void) { return 0; }
int sceAudiodecTermLibrary(void) { return 0; }
int sceAudiodecCreateDecoder(void) { return 0; }
int sceAudiodecDeleteDecoder(void) { return 0; }
int sceAudiodecDecode(void) { return 0; }
