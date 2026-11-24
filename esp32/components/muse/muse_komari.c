/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Komari page (CONFIG_MUSE_KOMARI_PAGE): rotates through the servers
 * of a Komari monitor panel, one server per screen, advancing every
 * muse_komari_rotate_s() seconds (a tap on the page jumps to the next
 * server at once). Data comes from the panel's public API:
 * GET {url}/api/nodes for the node list (name, region, totals, plan
 * expiry) and GET {url}/api/recent/{uuid} per node for the live
 * status. A node counts as online when its last report is under 90 s
 * old. Country flags are pre-rendered bitmaps (komari_flags.c); a
 * region without a bundled flag falls back to its two-letter code.
 *
 * Same shape as muse_home.c: a fetch task fills s_nodes under a mutex
 * and raises s_dirty; the 1 s LVGL timer does all drawing.
 */

#include "muse_komari.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "komari_flags.h"
#include "muse_lang.h"
#include "muse_wifi.h"
#include "muse_input.h"

static const char *TAG = "muse_komari";

/* Same palette as the home page. */
#define COL_BG      0x0a141f
#define COL_PANEL   0x14304a
#define COL_PANEL2  0x102539
#define COL_TEXT    0xffffff
#define COL_DIM     0x9db4c0
#define COL_ONLINE  0x3fb950
#define COL_OFFLINE 0xe5534b
#define COL_CPU     0x2f81f7
#define COL_MEM     0x3fb950
#define COL_DISK    0xd29922

#define MAX_NODES 32
#define URL_MAX   128

typedef struct {
    char uuid[40];
    char name[64];
    char code[3];              /* ISO alpha-2 from the region, or "" */
    uint64_t mem_total, disk_total;
    int64_t expires;           /* epoch, 0 = panel gives none */
    bool have;                 /* a status record was fetched */
    float cpu;                 /* percent */
    uint64_t ram_used, disk_used;
    uint64_t net_up, net_down; /* bytes per second */
    double load1;
    int64_t uptime;            /* seconds */
    int64_t updated;           /* epoch of the last report */
    bool online;
} knode_t;

static knode_t s_nodes[MAX_NODES];      /* fetch task -> UI */
static int s_count;
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty;
static int64_t s_nodes_at;              /* when /api/nodes last ran */
static int64_t s_last_ok;               /* last full status round */

static char s_url[URL_MAX];
static int s_rotate_s = 10;
static bool s_cfg_loaded;
static volatile bool s_refetch;         /* URL changed: fetch now */
static volatile bool s_page_active;     /* this tile is showing */
static volatile bool s_refreshing;      /* a forced refresh is running */
static int s_idx;                       /* node on screen (UI-owned) */

/* ------------------------------------------------------------------ */
/* Small helpers (mirrors of muse_home.c's)                            */
/* ------------------------------------------------------------------ */

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

#if CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define KM_CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define KM_CJK_FONT muse_font_cjk_16
#endif

static const lv_font_t *km_font(const lv_font_t *plain)
{
#ifdef KM_CJK_FONT
    static lv_font_t s_copies[4];
    static const lv_font_t *s_plains[4];
    for (int i = 0; i < 4; i++) {
        if (s_plains[i] == plain) {
            return &s_copies[i];
        }
        if (!s_plains[i]) {
            s_plains[i] = plain;
            s_copies[i] = *plain;
            s_copies[i].fallback = &KM_CJK_FONT;
            return &s_copies[i];
        }
    }
#endif
    return plain;
}

static void set_cjk(lv_obj_t *l, const char *text, const lv_font_t *plain)
{
    lv_obj_set_style_text_font(l, km_font(plain), 0);
    lv_label_set_text(l, text);
}

/* Net-rate arrows: drawn bitmaps (red up, green down), since
 * no available font carries arrow glyphs. Pre-blended onto
 * the tile background, same as the flags. */
static const uint8_t arrow_up_map[] = {
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0x04, 0x49,
    0x25, 0x59, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xe4, 0x30, 0x07, 0xf2, 0x27, 0xfa, 0x04, 0x41,
    0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xc4, 0x20,
    0x07, 0xe2, 0x27, 0xfa, 0x27, 0xfa, 0x27, 0xf2, 0xe4, 0x30, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xc4, 0x18, 0xe6, 0xd1, 0x47, 0xfa, 0x07, 0xfa,
    0x27, 0xfa, 0x27, 0xfa, 0x07, 0xe2, 0xc4, 0x28, 0xa3, 0x08, 0xa4, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x10,
    0xc6, 0xc1, 0x47, 0xfa, 0x27, 0xfa, 0x27, 0xfa, 0x27, 0xfa, 0x07, 0xfa,
    0x47, 0xfa, 0xe6, 0xd9, 0xc4, 0x20, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa4, 0x08, 0xa4, 0x10, 0xa6, 0xb1, 0x47, 0xfa, 0x07, 0xfa,
    0x27, 0xfa, 0x27, 0xfa, 0x27, 0xfa, 0x27, 0xfa, 0x07, 0xfa, 0x47, 0xfa,
    0xe6, 0xc9, 0xc4, 0x18, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x10, 0xe4, 0x30, 0xe4, 0x28, 0xe4, 0x28, 0xc6, 0xc9, 0x47, 0xfa,
    0x27, 0xfa, 0xe6, 0xd9, 0xe4, 0x28, 0xc4, 0x28, 0xe4, 0x30, 0xa4, 0x18,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa6, 0xb9, 0x47, 0xfa, 0x27, 0xfa, 0xe6, 0xd9,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08,
    0xc6, 0xc1, 0x47, 0xfa, 0x27, 0xfa, 0xe6, 0xd9, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xc6, 0xc1, 0x47, 0xfa,
    0x27, 0xfa, 0xe6, 0xd9, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xc6, 0xc1, 0x47, 0xfa, 0x27, 0xfa, 0xe6, 0xd9,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08,
    0xc6, 0xb9, 0x27, 0xfa, 0x27, 0xfa, 0xe6, 0xd9, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xc6, 0xc1, 0x47, 0xfa,
    0x47, 0xfa, 0xe6, 0xe1, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0x86, 0xa1, 0xe7, 0xd1, 0xe6, 0xd1, 0xa6, 0xb1,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
};
static const lv_image_dsc_t arrow_up = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .w = 16, .h = 16, .stride = 32 },
    .data_size = sizeof(arrow_up_map),
    .data = arrow_up_map,
};

static const uint8_t arrow_dn_map[] = {
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0x67, 0x24, 0xe8, 0x2d,
    0xc8, 0x2d, 0xe7, 0x24, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0x48, 0x2d, 0x4a, 0x37, 0x2a, 0x37, 0xe8, 0x2d,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08,
    0x28, 0x25, 0xe9, 0x36, 0xc9, 0x36, 0xc8, 0x2d, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0x28, 0x25, 0x09, 0x37,
    0xe9, 0x36, 0xc8, 0x2d, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0x28, 0x25, 0x09, 0x37, 0xe9, 0x36, 0xc8, 0x2d,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xc4, 0x08, 0xa4, 0x08,
    0x28, 0x25, 0x09, 0x37, 0xe9, 0x36, 0xc8, 0x2d, 0xa4, 0x08, 0xa4, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0x07, 0x25, 0x09, 0x37,
    0xe9, 0x36, 0xc8, 0x2d, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xc4, 0x08, 0xc4, 0x11,
    0x84, 0x09, 0x84, 0x11, 0x68, 0x2d, 0xe9, 0x36, 0xc9, 0x36, 0xe8, 0x2d,
    0x84, 0x11, 0x64, 0x11, 0xc4, 0x11, 0x04, 0x09, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa4, 0x08, 0xc4, 0x08, 0xe7, 0x24, 0x4a, 0x37, 0x89, 0x36,
    0xa9, 0x36, 0xa9, 0x36, 0xa9, 0x36, 0xa9, 0x36, 0x89, 0x36, 0x2a, 0x37,
    0x88, 0x2d, 0x04, 0x09, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xe4, 0x08, 0x48, 0x2d, 0x2a, 0x37, 0x89, 0x36, 0xa9, 0x36,
    0xa9, 0x36, 0x89, 0x36, 0x09, 0x37, 0xe8, 0x2d, 0x24, 0x09, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08,
    0x04, 0x09, 0xc8, 0x2d, 0x09, 0x37, 0xa9, 0x36, 0x89, 0x36, 0xe9, 0x36,
    0x49, 0x2e, 0x64, 0x11, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0x24, 0x09,
    0x29, 0x2e, 0xe9, 0x36, 0xa9, 0x36, 0xa9, 0x36, 0xa4, 0x11, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa4, 0x11, 0x69, 0x36,
    0xe9, 0x36, 0x25, 0x12, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0x25, 0x12, 0xa5, 0x12, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa4, 0x08, 0xa3, 0x08, 0xa3, 0x08,
    0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08, 0xa3, 0x08,
};
static const lv_image_dsc_t arrow_dn = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .w = 16, .h = 16, .stride = 32 },
    .data_size = sizeof(arrow_dn_map),
    .data = arrow_dn_map,
};

static void fmt_bytes(uint64_t v, char *buf, size_t n)
{
    if (v >= (1ull << 30)) {
        snprintf(buf, n, "%.1fG", (double)v / 1073741824.0);
    } else if (v >= (1ull << 20)) {
        snprintf(buf, n, "%.0fM", (double)v / 1048576.0);
    } else if (v >= (1ull << 10)) {
        snprintf(buf, n, "%.0fK", (double)v / 1024.0);
    } else {
        snprintf(buf, n, "%lluB", (unsigned long long)v);
    }
}

/* Days from civil (Howard Hinnant's algorithm): ISO timestamps from
 * the panel are UTC, and the board's TZ is CST-8, so mktime() would
 * skew them; convert directly. */
static int64_t civil_epoch(int y, int m, int d, int hh, int mm, int ss)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

static int64_t parse_iso(const char *s)
{
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi,
                     &se) < 3) {
        return 0;
    }
    return civil_epoch(y, mo, d, h, mi, se);
}

/* Komari's region is a flag emoji (two regional indicators) or, on
 * some panels, a plain alpha-2 code. Reduce both to "JP" form. */
static void region_code(const char *region, char out[3])
{
    out[0] = out[1] = out[2] = '\0';
    if (!region) {
        return;
    }
    const uint8_t *u = (const uint8_t *)region;
    if (u[0] == 0xF0 && u[1] == 0x9F && u[2] == 0x87 &&
        u[3] >= 0xA6 && u[3] <= 0xBF &&
        u[4] == 0xF0 && u[5] == 0x9F && u[6] == 0x87 &&
        u[7] >= 0xA6 && u[7] <= 0xBF) {
        out[0] = (char)('A' + (u[3] - 0xA6));
        out[1] = (char)('A' + (u[7] - 0xA6));
        return;
    }
    if (region[0] && region[1] && !region[2]) {
        out[0] = (char)(region[0] >= 'a' && region[0] <= 'z'
                        ? region[0] - 32 : region[0]);
        out[1] = (char)(region[1] >= 'a' && region[1] <= 'z'
                        ? region[1] - 32 : region[1]);
    }
}

/* Node names arrive with stray leading whitespace (one of the user's
 * literally starts with a tab). Trim, and truncate on a UTF-8
 * boundary. */
static void clean_name(const char *src, char *dst, size_t cap)
{
    if (!src) {
        src = "";
    }
    while (*src == ' ' || *src == '\t') {
        src++;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) {
        dst[--n] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Configuration (NVS "musekomari")                                    */
/* ------------------------------------------------------------------ */

static void url_normalize(char *u)
{
    while (*u == ' ' || *u == '\t') {
        memmove(u, u + 1, strlen(u));
    }
    size_t n = strlen(u);
    while (n > 0 && (u[n - 1] == ' ' || u[n - 1] == '\t' ||
                     u[n - 1] == '/')) {
        u[--n] = '\0';
    }
}

static void cfg_load(void)
{
    if (s_cfg_loaded) {
        return;
    }
    s_cfg_loaded = true;
    strlcpy(s_url, CONFIG_MUSE_KOMARI_URL, sizeof(s_url));
    url_normalize(s_url);
    s_rotate_s = 10;
    nvs_handle_t h;
    if (nvs_open("musekomari", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_url);
        if (nvs_get_str(h, "url", s_url, &len) != ESP_OK) {
            strlcpy(s_url, CONFIG_MUSE_KOMARI_URL, sizeof(s_url));
            url_normalize(s_url);
        }
        uint8_t v;
        if (nvs_get_u8(h, "rot", &v) == ESP_OK &&
            (v == 5 || v == 10 || v == 15 || v == 30 || v == 60)) {
            s_rotate_s = v;
        }
        nvs_close(h);
    }
}

const char *muse_komari_url(void)
{
    cfg_load();
    return s_url;
}

void muse_komari_set_url(const char *url)
{
    cfg_load();
    char buf[URL_MAX];
    strlcpy(buf, url ? url : "", sizeof(buf));
    url_normalize(buf);
    if (strcmp(buf, s_url) == 0) {
        return;
    }
    strlcpy(s_url, buf, sizeof(s_url));
    nvs_handle_t h;
    if (nvs_open("musekomari", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "url", s_url);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_count = 0;            /* the old panel's nodes are wrong now */
        s_nodes_at = 0;
        s_last_ok = 0;
        s_dirty = true;
        xSemaphoreGive(s_lock);
    }
    s_refetch = true;
    ESP_LOGI(TAG, "panel -> %s", s_url);
}

int muse_komari_rotate_s(void)
{
    cfg_load();
    return s_rotate_s;
}

void muse_komari_set_rotate_s(int secs)
{
    cfg_load();
    if (secs != 5 && secs != 10 && secs != 15 && secs != 30 &&
        secs != 60) {
        return;
    }
    s_rotate_s = secs;
    nvs_handle_t h;
    if (nvs_open("musekomari", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rot", (uint8_t)secs);
        nvs_commit(h);
        nvs_close(h);
    }
}

int muse_komari_node_count(void)
{
    return s_count;
}

void muse_komari_status(char *buf, size_t len)
{
    cfg_load();
    if (!s_url[0]) {
        snprintf(buf, len, "No panel set");
    } else if (!s_last_ok) {
        snprintf(buf, len, "Not fetched yet");
    } else {
        struct tm tmv;
        time_t t = (time_t)s_last_ok;
        localtime_r(&t, &tmv);
        snprintf(buf, len, "%d nodes, updated %02d:%02d", s_count,
                 tmv.tm_hour, tmv.tm_min);
    }
}

/* ------------------------------------------------------------------ */
/* Fetch task                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len, cap;
} http_acc_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_acc_t *acc = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (acc->len + evt->data_len >= acc->cap) {
            return ESP_OK;   /* truncated: the JSON parse will fail */
        }
        memcpy(acc->buf + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
        acc->buf[acc->len] = '\0';
    }
    return ESP_OK;
}

/* One client per fetch cycle, reused for every request: a fresh
 * TLS handshake per node made a full refresh take ~1 min (the
 * PSRAM-backed TLS buffers make each handshake seconds slow),
 * while the requests themselves are small. The client is closed
 * at the end of each cycle (http_cycle_end), so no stale
 * connection is carried across the minutes-long wait between
 * cycles; a reused connection that does go stale mid-cycle is
 * retried once on a fresh one. */
static esp_http_client_handle_t s_client;
static http_acc_t s_acc;

static void http_client_close(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static void http_cycle_end(void)
{
    http_client_close();
}

static char *http_get(const char *url, size_t cap)
{
    s_acc.cap = cap;
    s_acc.len = 0;
    s_acc.buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_acc.buf) {
        return NULL;
    }
    s_acc.buf[0] = '\0';
    for (int attempt = 0; attempt < 2; attempt++) {
        bool fresh = s_client == NULL;
        if (fresh) {
            esp_http_client_config_t cfg = {
                .url = url,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 15000,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .event_handler = http_event,
                .user_data = &s_acc,
                .keep_alive_enable = true,
            };
            s_client = esp_http_client_init(&cfg);
            if (!s_client) {
                break;
            }
        } else {
            esp_http_client_set_url(s_client, url);
            s_acc.len = 0;
            s_acc.buf[0] = '\0';
        }
        esp_err_t err = esp_http_client_perform(s_client);
        int status = esp_http_client_get_status_code(s_client);
        if (err == ESP_OK && status == 200 && s_acc.len > 0) {
            char *body = s_acc.buf;
            s_acc.buf = NULL;   /* ownership passes to the caller */
            return body;
        }
        ESP_LOGW(TAG, "GET %s -> err %s, HTTP %d, %u bytes", url,
                 esp_err_to_name(err), status, (unsigned)s_acc.len);
        http_client_close();
        if (fresh) {
            break;   /* a fresh connection failed too: a real error */
        }
        /* A reused connection went stale: retry once, fresh. */
    }
    free(s_acc.buf);
    s_acc.buf = NULL;
    return NULL;
}

static bool num_field(cJSON *obj, const char *name, double *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

static bool fetch_nodes(void)
{
    char url[URL_MAX + 16];
    snprintf(url, sizeof url, "%s/api/nodes", s_url);
    char *body = http_get(url, 48 * 1024);
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return false;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = cJSON_IsArray(data);
    knode_t *fresh = NULL;
    if (ok) {
        /* 32 nodes do not fit on the task stack; PSRAM it is. */
        fresh = heap_caps_malloc(sizeof(knode_t) * MAX_NODES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!fresh) {
            cJSON_Delete(root);
            return false;
        }
        int n = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, data) {
            if (n >= MAX_NODES) {
                break;
            }
            cJSON *uuid = cJSON_GetObjectItemCaseSensitive(item, "uuid");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!cJSON_IsString(uuid) || !uuid->valuestring) {
                continue;
            }
            knode_t *kn = &fresh[n];
            memset(kn, 0, sizeof *kn);
            strlcpy(kn->uuid, uuid->valuestring, sizeof(kn->uuid));
            clean_name(cJSON_IsString(name) ? name->valuestring : "",
                       kn->name, sizeof(kn->name));
            cJSON *region = cJSON_GetObjectItemCaseSensitive(item,
                                                             "region");
            region_code(cJSON_IsString(region) ? region->valuestring
                                               : "",
                        kn->code);
            double v;
            if (num_field(item, "mem_total", &v)) {
                kn->mem_total = (uint64_t)v;
            }
            if (num_field(item, "disk_total", &v)) {
                kn->disk_total = (uint64_t)v;
            }
            cJSON *exp = cJSON_GetObjectItemCaseSensitive(item,
                                                          "expired_at");
            if (cJSON_IsString(exp)) {
                kn->expires = parse_iso(exp->valuestring);
            }
            n++;
        }
        /* Merge: keep the fetched status of nodes that persist. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < s_count; j++) {
                if (strcmp(fresh[i].uuid, s_nodes[j].uuid) == 0) {
                    knode_t keep = fresh[i];
                    keep.have = s_nodes[j].have;
                    keep.cpu = s_nodes[j].cpu;
                    keep.ram_used = s_nodes[j].ram_used;
                    keep.disk_used = s_nodes[j].disk_used;
                    keep.net_up = s_nodes[j].net_up;
                    keep.net_down = s_nodes[j].net_down;
                    keep.load1 = s_nodes[j].load1;
                    keep.uptime = s_nodes[j].uptime;
                    keep.updated = s_nodes[j].updated;
                    keep.online = s_nodes[j].online;
                    fresh[i] = keep;
                    break;
                }
            }
        }
        memcpy(s_nodes, fresh, sizeof(knode_t) * n);
        s_count = n;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "nodes: %d", n);
    }
    free(fresh);
    cJSON_Delete(root);
    return ok;
}

/* Fills the status fields of *st from one /api/recent round. */
static bool fetch_recent(const char *uuid, knode_t *st)
{
    char url[URL_MAX + 64];
    snprintf(url, sizeof url, "%s/api/recent/%s", s_url, uuid);
    char *body = http_get(url, 24 * 1024);
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return false;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *rec = NULL;
    if (cJSON_IsArray(data)) {
        int n = cJSON_GetArraySize(data);
        if (n > 0) {
            rec = cJSON_GetArrayItem(data, n - 1);
        }
    } else if (cJSON_IsObject(data)) {
        rec = data;
    }
    bool ok = rec != NULL;
    if (ok) {
        double v;
        cJSON *cpu = cJSON_GetObjectItemCaseSensitive(rec, "cpu");
        if (cpu && num_field(cpu, "usage", &v)) {
            st->cpu = (float)v;
        }
        cJSON *ram = cJSON_GetObjectItemCaseSensitive(rec, "ram");
        if (ram && num_field(ram, "used", &v)) {
            st->ram_used = (uint64_t)v;
        }
        cJSON *disk = cJSON_GetObjectItemCaseSensitive(rec, "disk");
        if (disk && num_field(disk, "used", &v)) {
            st->disk_used = (uint64_t)v;
        }
        cJSON *net = cJSON_GetObjectItemCaseSensitive(rec, "network");
        if (net) {
            if (num_field(net, "up", &v)) {
                st->net_up = (uint64_t)v;
            }
            if (num_field(net, "down", &v)) {
                st->net_down = (uint64_t)v;
            }
        }
        cJSON *load = cJSON_GetObjectItemCaseSensitive(rec, "load");
        if (load) {
            num_field(load, "load1", &st->load1);
        }
        if (num_field(rec, "uptime", &v)) {
            st->uptime = (int64_t)v;
        }
        cJSON *upd = cJSON_GetObjectItemCaseSensitive(rec, "updated_at");
        if (cJSON_IsString(upd)) {
            st->updated = parse_iso(upd->valuestring);
        }
        st->have = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void km_task(void *arg)
{
    (void)arg;
    /* See muse_home: keep this page's first TLS out of the boot
     * window (and behind home's), or it can fail mbedtls setup
     * (-0x008D) against the Link connects. */
    vTaskDelay(pdMS_TO_TICKS(14000));
    for (;;) {
        if (!muse_wifi_connected() || !s_url[0]) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        /* Consume a pending forced refresh (BOOT on the tile, or
         * a settings URL change) up front: a press during this
         * cycle sets s_refetch again and is honoured by the next
         * one. A forced cycle re-pulls every node's /api/recent
         * (that is where the probe metrics live); the node list
         * carries only names and plans, so it is refetched only
         * when missing or stale — re-pulling it per press cost a
         * full request for nothing. */
        bool forced = s_refetch;
        s_refetch = false;
        int64_t cyc0 = esp_timer_get_time();
        int64_t now = time(NULL);
        if (s_count == 0 || now - s_nodes_at > 3600) {
            if (fetch_nodes()) {
                s_nodes_at = time(NULL);
            }
        }
        bool ok = s_count > 0;
        int n = s_count;
        /* Start from the node on screen: its fresh values land
         * after one request, the rest follow in rotation order. */
        int start = s_idx;
        if (start < 0 || start >= n) {
            start = 0;
        }
        for (int k = 0; k < n && !s_refetch; k++) {
            int i = (start + k) % n;
            char uuid[40];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (i >= s_count) {
                xSemaphoreGive(s_lock);
                break;
            }
            strlcpy(uuid, s_nodes[i].uuid, sizeof(uuid));
            xSemaphoreGive(s_lock);
            knode_t st = {0};
            if (fetch_recent(uuid, &st)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                for (int j = 0; j < s_count; j++) {
                    if (strcmp(s_nodes[j].uuid, uuid) == 0) {
                        knode_t *t = &s_nodes[j];
                        t->have = true;
                        t->cpu = st.cpu;
                        t->ram_used = st.ram_used;
                        t->disk_used = st.disk_used;
                        t->net_up = st.net_up;
                        t->net_down = st.net_down;
                        t->load1 = st.load1;
                        t->uptime = st.uptime;
                        t->updated = st.updated;
                        int64_t tnow = time(NULL);
                        t->online = st.updated > 0 &&
                            (tnow < 1704067200 ||       /* unsynced */
                             tnow - st.updated <= 90);
                        break;
                    }
                }
                xSemaphoreGive(s_lock);
                s_dirty = true;   /* show this node as it lands */
            } else {
                ok = false;
            }
        }
        if (ok && !s_refetch) {
            s_last_ok = time(NULL);
        }
        if (forced) {
            ESP_LOGI(TAG, "refresh cycle: %d nodes, ok=%d, %lld ms",
                     n, (int)ok,
                     (long long)((esp_timer_get_time() - cyc0) / 1000));
            if (s_refreshing) {
                s_refreshing = false;   /* state line back to normal */
                s_dirty = true;
            }
        }
        if (ok || s_count > 0) {
            s_dirty = true;
        }
        http_cycle_end();   /* one TLS handshake per cycle, not per node */
        int wait_s = ok ? CONFIG_MUSE_KOMARI_REFRESH_S : 60;
        for (int t = 0; t < wait_s && !s_refetch; t++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ------------------------------------------------------------------ */
/* UI (LVGL context only)                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_tile;
static lv_obj_t *s_content;
static lv_obj_t *s_flag_img;
static lv_obj_t *s_code_chip;
static lv_obj_t *s_code_lbl;
static lv_obj_t *s_name_lbl;
static lv_obj_t *s_count_lbl;
static lv_obj_t *s_dot;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_empty_lbl;
static lv_obj_t *s_bars[3];
static lv_obj_t *s_bar_vals[3];
static lv_obj_t *s_up_lbl;
static lv_obj_t *s_dn_lbl;
static lv_obj_t *s_load_lbl;
static lv_obj_t *s_exp_lbl;
static int64_t s_last_switch;
static int64_t s_hold_until;

static void fmt_uptime(int64_t s, char *buf, size_t n)
{
    if (s >= 86400) {
        snprintf(buf, n, "%lld天%lld时", (long long)(s / 86400),
                 (long long)(s % 86400 / 3600));
    } else if (s >= 3600) {
        snprintf(buf, n, "%lld时%lld分", (long long)(s / 3600),
                 (long long)(s % 3600 / 60));
    } else {
        snprintf(buf, n, "%lld分", (long long)(s / 60));
    }
}

static void render(int idx)
{
    knode_t nd;
    int count;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_count;
    if (count > 0) {
        if (idx < 0 || idx >= count) {
            idx = 0;
        }
        nd = s_nodes[idx];
    }
    xSemaphoreGive(s_lock);
    if (count == 0) {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        set_cjk(s_empty_lbl,
                s_url[0] ? "KOMARI 加载中" : "未设置面板地址",
                &lv_font_montserrat_20);
        lv_obj_remove_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    s_idx = idx;
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    char buf[128], a[32], b[32];

    const lv_image_dsc_t *flag = komari_flag(nd.code);
    if (flag) {
        lv_image_set_src(s_flag_img, flag);
        lv_obj_remove_flag(s_flag_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_code_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_flag_img, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_code_lbl, nd.code[0] ? nd.code : "--");
        lv_obj_remove_flag(s_code_chip, LV_OBJ_FLAG_HIDDEN);
    }
    set_cjk(s_name_lbl, nd.name, &lv_font_montserrat_16);
    snprintf(buf, sizeof buf, "%d/%d", idx + 1, count);
    lv_label_set_text(s_count_lbl, buf);
    lv_obj_set_style_bg_color(s_dot,
                              lv_color_hex(nd.online ? COL_ONLINE
                                                     : COL_OFFLINE),
                              0);
    int64_t now = time(NULL);
    bool synced = now >= 1704067200;
    if (s_refreshing) {
        set_cjk(s_state_lbl, "刷新中...", &lv_font_montserrat_14);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(COL_DIM),
                                    0);
    } else if (!nd.have) {
        set_cjk(s_state_lbl, "暂无数据", &lv_font_montserrat_14);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(COL_DIM),
                                    0);
    } else if (nd.online) {
        fmt_uptime(nd.uptime, a, sizeof a);
        snprintf(buf, sizeof buf, "ONLINE 已运行 %s", a);
        set_cjk(s_state_lbl, buf, &lv_font_montserrat_14);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(COL_DIM),
                                    0);
    } else {
        if (synced && nd.updated > 0) {
            int64_t ago = now - nd.updated;
            if (ago >= 86400) {
                snprintf(a, sizeof a, "%lld天前", (long long)(ago / 86400));
            } else if (ago >= 3600) {
                snprintf(a, sizeof a, "%lld小时前", (long long)(ago / 3600));
            } else {
                snprintf(a, sizeof a, "%lld分钟前",
                         (long long)(ago / 60 + 1));
            }
            snprintf(buf, sizeof buf, "OFFLINE %s", a);
        } else {
            snprintf(buf, sizeof buf, "OFFLINE");
        }
        set_cjk(s_state_lbl, buf, &lv_font_montserrat_14);
        lv_obj_set_style_text_color(s_state_lbl,
                                    lv_color_hex(COL_OFFLINE), 0);
    }

    int pct[3];
    pct[0] = (int)(nd.cpu + 0.5f);
    pct[1] = nd.mem_total ? (int)(nd.ram_used * 100 / nd.mem_total) : 0;
    pct[2] = nd.disk_total ? (int)(nd.disk_used * 100 / nd.disk_total)
                           : 0;
    for (int i = 0; i < 3; i++) {
        if (pct[i] < 0) {
            pct[i] = 0;
        }
        if (pct[i] > 100) {
            pct[i] = 100;
        }
        lv_bar_set_value(s_bars[i], nd.have ? pct[i] : 0, LV_ANIM_OFF);
    }
    if (nd.have) {
        snprintf(buf, sizeof buf, "%d%%", pct[0]);
        lv_label_set_text(s_bar_vals[0], buf);
        fmt_bytes(nd.ram_used, a, sizeof a);
        fmt_bytes(nd.mem_total, b, sizeof b);
        snprintf(buf, sizeof buf, "%d%% %s/%s", pct[1], a, b);
        lv_label_set_text(s_bar_vals[1], buf);
        fmt_bytes(nd.disk_used, a, sizeof a);
        fmt_bytes(nd.disk_total, b, sizeof b);
        snprintf(buf, sizeof buf, "%d%% %s/%s", pct[2], a, b);
        lv_label_set_text(s_bar_vals[2], buf);
        fmt_bytes(nd.net_up, a, sizeof a);
        snprintf(buf, sizeof buf, "%s/s", a);
        lv_label_set_text(s_up_lbl, buf);
        fmt_bytes(nd.net_down, b, sizeof b);
        snprintf(buf, sizeof buf, "%s/s", b);
        lv_label_set_text(s_dn_lbl, buf);
        snprintf(buf, sizeof buf, "LOAD %.2f", nd.load1);
        lv_label_set_text(s_load_lbl, buf);
    } else {
        for (int i = 0; i < 3; i++) {
            lv_label_set_text(s_bar_vals[i], "--");
        }
        lv_label_set_text(s_up_lbl, "--");
        lv_label_set_text(s_dn_lbl, "--");
        lv_label_set_text(s_load_lbl, "");
    }

    if (nd.expires > 0 && synced) {
        struct tm tmv;
        time_t et = (time_t)nd.expires;
        localtime_r(&et, &tmv);
        int64_t days = (nd.expires - now) / 86400;
        if (days >= 0) {
            snprintf(buf, sizeof buf, "到期 %04d-%02d-%02d 剩 %lld 天",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     (long long)days);
            lv_obj_set_style_text_color(s_exp_lbl,
                                        lv_color_hex(COL_DIM), 0);
        } else {
            snprintf(buf, sizeof buf, "已过期 %lld 天",
                     (long long)(-days));
            lv_obj_set_style_text_color(s_exp_lbl,
                                        lv_color_hex(COL_OFFLINE), 0);
        }
        set_cjk(s_exp_lbl, buf, &lv_font_montserrat_14);
    } else {
        set_cjk(s_exp_lbl, "到期 --", &lv_font_montserrat_14);
        lv_obj_set_style_text_color(s_exp_lbl, lv_color_hex(COL_DIM),
                                    0);
    }
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    if (s_count < 1) {
        return;
    }
    int64_t now = time(NULL);
    s_last_switch = now;
    s_hold_until = now + (s_rotate_s * 3) / 2;
    render(s_idx + 1 >= s_count ? 0 : s_idx + 1);
}

static void km_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_dirty) {
        s_dirty = false;
        render(s_idx);
    }
    lv_obj_t *tv = s_tile ? lv_obj_get_parent(s_tile) : NULL;
    s_page_active = tv && lv_tileview_get_tile_active(tv) == s_tile;
    if (!s_page_active || s_count < 2) {
        return;
    }
    int64_t now = time(NULL);
    if (now >= s_hold_until && now - s_last_switch >= s_rotate_s) {
        s_last_switch = now;
        render(s_idx + 1 >= s_count ? 0 : s_idx + 1);
    }
}

/* ---------- input hook: BOOT refreshes while this tile shows ---- */

static volatile bool s_km_divert_up;

static bool km_ptt_hook(const muse_input_event_t *ev)
{
    if (ev->wake) return false;   /* a wake tap only wakes */
    if (ev->type == MUSE_PTT_UP) {
        bool d = s_km_divert_up;
        s_km_divert_up = false;
        return d;
    }
    if (ev->type != MUSE_PTT_DOWN) return false;
    if (!s_page_active) return false;
    s_km_divert_up = true;
    s_refetch = true;      /* km_task drops its wait and refetches */
    s_refreshing = true;   /* state line shows it until the cycle ends */
    s_dirty = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

void muse_komari_build(lv_obj_t *tile)
{
    cfg_load();
    s_tile = tile;
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    s_content = lv_obj_create(tile);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, 320, 240);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_SCROLLABLE);

    s_flag_img = lv_image_create(s_content);
    lv_obj_set_pos(s_flag_img, 10, 10);
    lv_obj_remove_flag(s_flag_img, LV_OBJ_FLAG_CLICKABLE);
    s_code_chip = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_code_chip);
    lv_obj_set_pos(s_code_chip, 10, 10);
    lv_obj_set_size(s_code_chip, 36, 24);
    lv_obj_set_style_bg_color(s_code_chip, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(s_code_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_code_chip, 4, 0);
    lv_obj_remove_flag(s_code_chip, LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_SCROLLABLE);
    s_code_lbl = lv_label_create(s_code_chip);
    lv_obj_set_style_text_font(s_code_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_code_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(s_code_lbl);

    s_name_lbl = mk_label(s_content, &lv_font_montserrat_16, COL_TEXT,
                          54, 13);
    lv_obj_set_width(s_name_lbl, 252);
    lv_obj_set_height(s_name_lbl, 22);
    lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_CLIP);
    s_dot = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_pos(s_dot, 12, 47);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_OFFLINE), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot, 5, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_SCROLLABLE);
    s_count_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_DIM,
                           250, 210);
    lv_obj_set_width(s_count_lbl, 60);
    lv_obj_set_style_text_align(s_count_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_state_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_DIM,
                           28, 43);

    static const char *const caps[3] = { "CPU", "MEM", "DISK" };
    static const uint32_t bar_cols[3] = { COL_CPU, COL_MEM, COL_DISK };
    for (int i = 0; i < 3; i++) {
        int y = 66 + i * 40;
        lv_obj_t *cap = mk_label(s_content, &lv_font_montserrat_14,
                                 COL_DIM, 12, y);
        lv_label_set_text(cap, caps[i]);
        s_bars[i] = lv_bar_create(s_content);
        lv_obj_set_pos(s_bars[i], 58, y + 2);
        lv_obj_set_size(s_bars[i], 252, 12);
        lv_bar_set_range(s_bars[i], 0, 100);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(COL_PANEL2),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(bar_cols[i]),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER,
                                LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bars[i], 6, LV_PART_INDICATOR);
        lv_obj_remove_flag(s_bars[i], LV_OBJ_FLAG_CLICKABLE);
        s_bar_vals[i] = mk_label(s_content, &lv_font_montserrat_14,
                                 COL_DIM, 58, y + 18);
        lv_obj_set_width(s_bar_vals[i], 252);
        lv_label_set_long_mode(s_bar_vals[i], LV_LABEL_LONG_MODE_CLIP);
    }
    lv_obj_t *up_img = lv_image_create(s_content);
    lv_image_set_src(up_img, &arrow_up);
    lv_obj_set_pos(up_img, 12, 190);
    lv_obj_remove_flag(up_img, LV_OBJ_FLAG_CLICKABLE);
    s_up_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_TEXT,
                        32, 188);
    lv_obj_set_width(s_up_lbl, 62);
    lv_label_set_long_mode(s_up_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_t *dn_img = lv_image_create(s_content);
    lv_image_set_src(dn_img, &arrow_dn);
    lv_obj_set_pos(dn_img, 102, 190);
    lv_obj_remove_flag(dn_img, LV_OBJ_FLAG_CLICKABLE);
    s_dn_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_TEXT,
                        122, 188);
    lv_obj_set_width(s_dn_lbl, 76);
    lv_label_set_long_mode(s_dn_lbl, LV_LABEL_LONG_MODE_CLIP);
    s_load_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_DIM,
                          0, 188);
    lv_obj_align(s_load_lbl, LV_ALIGN_TOP_RIGHT, -12, 188);
    s_exp_lbl = mk_label(s_content, &lv_font_montserrat_14, COL_DIM,
                         12, 212);

    s_empty_lbl = mk_label(tile, &lv_font_montserrat_20, COL_DIM, 0,
                           104);
    lv_obj_set_width(s_empty_lbl, 320);
    lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* A transparent catcher over the whole tile: taps jump to the
     * next server, drags still belong to the tileview (the same
     * pattern as the settings rows inside their scrolling list). */
    lv_obj_t *catcher = lv_obj_create(tile);
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, 320, 240);
    lv_obj_add_event_cb(catcher, on_tap, LV_EVENT_CLICKED, NULL);

    s_lock = xSemaphoreCreateMutex();
    s_last_switch = time(NULL);
    render(0);
    lv_timer_create(km_tick, 1000, NULL);
    muse_input_add_ptt_hook(km_ptt_hook);
    xTaskCreateWithCaps(km_task, "komari", 10240, NULL, 5, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "komari page built, panel %s",
             s_url[0] ? s_url : "(none)");
}
