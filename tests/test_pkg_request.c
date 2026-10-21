/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update/pkg_request.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    sf_pkg_request r = {0};
    r.pid = 276;
    strcpy(r.title, "PPSA99001");
    strcpy(r.sha256, "37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f");
    r.size = 25053222;
    strcpy(r.version, "01.000.015");
    char text[SF_PKG_REQUEST_MAX];
    const int n = sf_pkg_request_format(&r, text, sizeof(text));
    assert(n > 0);
    sf_pkg_request back;
    assert(sf_pkg_request_parse(text, (size_t)n, &back) == 0);
    assert(back.pid == 276 && !strcmp(back.title, "PPSA99001") && back.size == 25053222 &&
           !strcmp(back.version, "01.000.015") && !strcmp(back.sha256, r.sha256));
    /* Anything not written by format() is refused. */
    char bad[SF_PKG_REQUEST_MAX];
    const char *variants[] = {
        "SLOPFIN-PKG-UPDATE-"
        "1\n0276\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n2505"
        "3222\n01.000.015\n",
        "SLOPFIN-PKG-UPDATE-"
        "1\n276\nPPSA9900\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n250532"
        "22\n01.000.015\n",
        "SLOPFIN-PKG-UPDATE-"
        "1\n276\nPPSA99001\n37E50CD9C0812AFFB51C720A47FC82038D31F22C0F68BFB48AB653BE4FD4795F\n25053"
        "222\n01.000.015\n",
        "SLOPFIN-PKG-UPDATE-"
        "1\n276\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053"
        "222\n1.15\n",
        "SLOPFIN-PKG-UPDATE-"
        "1\n276\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053"
        "222\n01.000.015\nextra\n",
        "SLOPFIN-PKG-UPDATE-"
        "1\n276\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053"
        "222\n01.000.015",
        "SLOPFIN-PKG-UPDATE-"
        "2\n276\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053"
        "222\n01.000.015\n",
        "SLOPFIN-PKG-UPDATE-1\n-"
        "5\nPPSA99001\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053222\n"
        "01.000.015\n",
        "SLOPFIN-PKG-UPDATE-1\n276\n../../"
        "etc\n37e50cd9c0812affb51c720a47fc82038d31f22c0f68bfb48ab653be4fd4795f\n25053222\n01.000."
        "015\n",
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i)
    {
        snprintf(bad, sizeof(bad), "%s", variants[i]);
        assert(sf_pkg_request_parse(bad, strlen(bad), &back) < 0);
    }
    r.size = 100;
    assert(sf_pkg_request_format(&r, text, sizeof(text)) < 0);
    /* The content ID check reads only a CNT container's own field. */
    unsigned char cnt[0x80] = {0};
    memcpy(cnt,
           "\x7f"
           "CNT",
           4);
    memcpy(cnt + 0x40, "UP9000-PPSA99001_00-SLOPFIN000000000", 36);
    assert(sf_pkg_content_matches(cnt, sizeof(cnt), "UP9000-PPSA99001_00-") == 0);
    assert(sf_pkg_content_matches(cnt, sizeof(cnt), "UP9000-PPSA99004_00-") < 0);
    cnt[0] = 0;
    assert(sf_pkg_content_matches(cnt, sizeof(cnt), "UP9000-PPSA99001_00-") < 0);
    puts("package update request: format, strict parse and content check passed");
    return 0;
}
