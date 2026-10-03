/*
 * SlopFin - read (and, when built with SET_HDR, write) the console's HDR setting.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings > Screen and Video > HDR is stored in the system registry. This
 * payload, sent over elfldr, dumps every VIDEOOUT registry entry to
 * /data/slopfin-videoout-registry.txt so a setting can be read without anyone
 * opening the menu. Key numbers are from ps5-payload-dev/linkdev regmgr.h, the
 * same source ps5-hdcp-off used for VIDEOOUT_hdcp_off_mode.
 *
 *
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sceRegMgrGetInt(long key, int *value);
int sceRegMgrSetInt(long key, int value);
int sceRegMgrGetBin(long key, void *buffer, long size);
int sceRegMgrGetIntInitVal(long key, int *value);

struct entry
{
    const char *name;
    long key;
    int binary; /* 4-byte binary entries are floats */
};

static const struct entry kEntries[] = {
    {"mode", 167968768, 0},
    {"color_depth", 168034304, 0},
    {"signal_range", 168165376, 0},
    {"screen_size", 168230912, 0},
    {"enable_cec", 168296448, 0},
    {"yuv_range", 168427520, 0},
    {"display_area", 168624128, 0},
    {"hdcp_off_mode", 168886272, 0},
    {"setting_options", 168951808, 0},
    {"hdcp_version", 169082880, 0},
    {"hdr", 169148416, 0},
    {"hdr_confirmed", 169213952, 0},
    {"enable_supersampling_mode", 169279488, 0},
    {"hdr_max_ff_tml", 169345024, 1},
    {"hdr_max_tml", 169410560, 1},
    {"hdr_min_tml", 169476096, 1},
    {"hdr_tv_category", 169541632, 0},
    {"display_area2", 169803776, 0},
    {"display_area_type", 169869312, 0},
    {"4k_transfer_rate", 169934848, 0},
    {"enable_hdmi_devicelink", 170000384, 0},
    {"hfr", 170131456, 0},
    {"disable_hdcp", 171048960, 0},
    {"force_hdr_cap", 171114496, 0},
    {"vrr", 175243264, 0},
    {"vrr_monitor_type", 175308800, 0},
};

#define VIDEOOUT_hdr 169148416L

int main(void)
{
    char text[4096];
    int used = 0;

#ifdef SET_HDR
    {
        int before = -1, after = -1;
        (void)sceRegMgrGetInt(VIDEOOUT_hdr, &before);
        const int rc = sceRegMgrSetInt(VIDEOOUT_hdr, SET_HDR);
        (void)sceRegMgrGetInt(VIDEOOUT_hdr, &after);
        used += snprintf(text + used, sizeof(text) - used, "set hdr %d: rc=0x%08x, %d -> %d\n\n", SET_HDR,
                         (unsigned)rc, before, after);
    }
#endif

    for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i)
    {
        const struct entry *e = &kEntries[i];
        if (e->binary)
        {
            unsigned char raw[4] = {0};
            float value = 0.0f;
            const int rc = sceRegMgrGetBin(e->key, raw, sizeof(raw));
            memcpy(&value, raw, sizeof(value));
            used += snprintf(text + used, sizeof(text) - used, "%-26s rc=0x%08x  %.4f  (%02x %02x %02x %02x)\n",
                             e->name, (unsigned)rc, value, raw[0], raw[1], raw[2], raw[3]);
        }
        else
        {
            int value = -1, initial = -1;
            const int rc = sceRegMgrGetInt(e->key, &value);
            const int init_rc = sceRegMgrGetIntInitVal(e->key, &initial);
            used += snprintf(text + used, sizeof(text) - used, "%-26s rc=0x%08x  %-6d factory %d (rc=0x%08x)\n",
                             e->name, (unsigned)rc, value, initial, (unsigned)init_rc);
        }
    }

    const int fd = open("/data/slopfin-videoout-registry.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        (void)write(fd, text, (size_t)used);
        (void)close(fd);
    }
    fputs(text, stdout);
    return 0;
}
