/*
 * SlopFin - link-only facade for the Videodec2 module.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The public payload SDK ships no stub for the hardware video decoder. These
 * empty definitions exist purely so the linker and the module writer can
 * attribute the imports; the console supplies the real implementation.
 */

int sceVideodec2QueryComputeMemoryInfo(void) { return 0; }
int sceVideodec2AllocateComputeQueue(void) { return 0; }
int sceVideodec2ReleaseComputeQueue(void) { return 0; }
int sceVideodec2QueryDecoderMemoryInfo(void) { return 0; }
int sceVideodec2CreateDecoder(void) { return 0; }
int sceVideodec2DeleteDecoder(void) { return 0; }
int sceVideodec2Decode(void) { return 0; }
int sceVideodec2Flush(void) { return 0; }
int sceVideodec2Reset(void) { return 0; }
