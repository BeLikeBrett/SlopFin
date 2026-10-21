/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Installs drakmor's ppr-patch A53 selector, which a plaintext native package
 * needs before the console can mount it. Upstream decides everything: it
 * refuses any firmware without an exact verified profile, leaves an installed
 * selector as it is, and reports the outcome as a console notification.
 *
 * Load once per boot, before any native package starts: changing the selector
 * while package reads are in flight can hang the console.
 */
#define main ppr_patch_main
#include "main.c"
#undef main

int main(void)
{
    char *args[] = {"slopfin-a53-selector", "--install", "--idle", NULL};
    return ppr_patch_main(3, args);
}
