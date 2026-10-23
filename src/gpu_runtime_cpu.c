/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(SLOPFIN_GPU_MENU) && !defined(SLOPFIN_HOST)
/* Mesa's address library uses Clang's CPU dispatch ABI. A host compiler-rt
 * archive can itself contain AVX-512 in its CPU detection constructor, before
 * detection has run. The PS5 build uses the baseline x86-64 implementations;
 * expose no optional CPU features, so dispatch cannot select a wider clone.
 * Vulkan GPU capabilities are queried independently from the GPU driver. */
__attribute__((visibility("hidden"))) unsigned int __cpu_model[4] = {0, 0, 0, 0};
__attribute__((visibility("hidden"))) int __cpu_indicator_init(void)
{
    return 0;
}
#endif
