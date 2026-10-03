/*
 * SlopFin - link-only facade for the CommonDialog module.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The public payload SDK ships no stub for libSceCommonDialog, but the
 * on-screen keyboard cannot start without its initializer. This file exists
 * only so the linker and the module writer know which module that symbol
 * belongs to; the console provides the real implementation at load time.
 */

void sceCommonDialogInitialize(void)
{
}
