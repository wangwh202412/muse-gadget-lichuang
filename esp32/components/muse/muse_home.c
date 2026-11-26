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
 * Home page (CONFIG_MUSE_HOME_PAGE): the tile the gadget boots into and
 * drifts back to when idle. Clock and date from SNTP (this module starts
 * it; the timezone is fixed to CST-8), weather and air quality from
 * Open-Meteo (no key) for the configured city, refreshed every
 * CONFIG_MUSE_HOME_REFRESH_MIN minutes. Layout follows a weather-clock
 * reference: city + time on top, a main panel with the current
 * temperature, condition, high/low, humidity and an AQI chip, and a
 * three-day forecast row along the bottom.
 *
 * The fetch runs on its own task and never touches LVGL; it fills
 * s_pending under a mutex and raises s_wx_dirty, and the 1 s LVGL timer
 * that drives the clock applies the new values.
 */

#include "muse_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "muse_ble.h"
#include "muse_lang.h"
#include "muse_settings.h"
#include "muse_status_icons.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#include "muse_wx_icons.h"

static const char *TAG = "muse_home";

/* The reference look: deep blue panels on near-black, white text. */
#define COL_BG      0x0a141f
#define COL_PANEL   0x14304a
#define COL_PANEL2  0x102539
#define COL_TEXT    0xffffff
#define COL_DIM     0x9db4c0
#define COL_LINE    0x2a4a66
#define COL_FAINT   0x4d6675   /* status icon when its thing is off / unavailable */

#define WX_DAYS 3
#define STAT_N 6   /* status icons left of the clock */
#define STAT_ICON_PX 20   /* SZPI status-bar icon size */

/* Current-condition weather icon (36x36, muse_wx_icons.h, pre-blended
 * onto COL_PANEL): one per WMO-code group, same groups as wmo_text. */

typedef struct {
    int code;          /* WMO weather code */
    float tmin, tmax;
    int year, mon, day;
} home_day_t;

typedef struct {
    bool valid;
    float cur_temp;
    int humidity;
    int cur_code;
    home_day_t days[WX_DAYS];   /* days[0] is today */
    bool aqi_valid;
    int aqi;
} home_wx_t;

static home_wx_t s_pending;             /* fetch task -> UI */
static SemaphoreHandle_t s_wx_lock;
static volatile bool s_wx_dirty;
static home_wx_t s_shown;               /* what the labels currently say */

static lv_obj_t *s_city_lbl;
static lv_obj_t *s_time_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_temp_lbl;
static lv_obj_t *s_wx_icon;    /* colorful condition icon, main panel */
static lv_obj_t *s_cond_lbl;
static lv_obj_t *s_hilo_lbl;
static lv_obj_t *s_hum_lbl;
static lv_obj_t *s_aqi_chip;
static lv_obj_t *s_aqi_lbl;
static lv_obj_t *s_day_wd[WX_DAYS];
static lv_obj_t *s_day_dt[WX_DAYS];
static lv_obj_t *s_day_cd[WX_DAYS];
static lv_obj_t *s_day_tp[WX_DAYS];
static int s_last_min = -1;
static bool s_clock_synced;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
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

/* Status icons, left of the clock: Wi-Fi, Bluetooth, speaker,
 * microphone, camera, SD card. LVGL symbol glyphs do not render in
 * lv_font_montserrat_16 on this build (photo 2026-11-25: the whole
 * row was blank), so every icon is a pre-blended RGB565 bitmap in
 * muse_status_icons.h, same technique as the Komari arrows; lit and
 * faint are separate bitmaps, swapped by update_status_icons.
 * Camera and SD stay faint: the camera backend is parked and there
 * is no SD driver yet. */
static lv_obj_t *s_stat[STAT_N];   /* [0] wifi [1] ble [2] speaker [3] mic [4] camera [5] sd */

/* Right-align the icon row against the clock, squeezing the pitch if
 * the city name runs long. SZPI's status bar (28 px, everything
 * vertically centered) is the reference: the row, the city and the
 * clock share one centerline -- the clock's, since it is the tallest
 * element. Label geometry reads as 0 until LVGL's first refresh, so
 * this runs again from the 1 s tick and fixes itself once sizes are
 * real (build-time call alone placed the row off-screen, -104). */
static void layout_status_icons(void)
{
    if (!s_stat[0] || !s_time_lbl || !s_city_lbl) return;
    int pitch = 22;
    int end = lv_obj_get_x(s_time_lbl) - 8;
    int start = end - STAT_N * pitch;
    int city_end = 12 + lv_obj_get_width(s_city_lbl) + 6;
    if (start < city_end) {
        pitch = (end - city_end) / STAT_N;
        if (pitch < STAT_ICON_PX) pitch = STAT_ICON_PX;
        start = end - STAT_N * pitch;
    }
    int clock_h = lv_obj_get_height(s_time_lbl);
    int cy = (clock_h > 0) ? lv_obj_get_y(s_time_lbl) + clock_h / 2 : 22;
    int icon_y = cy - STAT_ICON_PX / 2;
    for (int i = 0; i < STAT_N; i++) {
        lv_obj_set_pos(s_stat[i], start + i * pitch, icon_y);
    }
    int city_h = lv_obj_get_height(s_city_lbl);
    if (city_h > 0) {
        lv_obj_set_y(s_city_lbl, cy - city_h / 2);
    }
}

static void update_status_icons(void)
{
    if (!s_stat[0]) return;
    lv_image_set_src(s_stat[0], muse_wifi_connected() ? &stat_wifi_lit
                                                      : &stat_wifi_faint);
    muse_ble_status_t bs;
    muse_ble_status(&bs);
    lv_image_set_src(s_stat[1], bs.state != MUSE_BLE_OFF ? &stat_ble_lit
                                                         : &stat_ble_faint);
    int vol = muse_settings_volume();
    lv_image_set_src(s_stat[2], vol == 0 ? &stat_spk_mute : &stat_spk_lit);
    lv_image_set_src(s_stat[3], muse_voice_mic_live() ? &stat_mic_lit
                                                      : &stat_mic_faint);
    /* [4] camera and [5] SD card stay faint (src set at build). */
}

/* Montserrat has no CJK glyphs, so a plain label font draws Chinese as
 * boxes. Chain the SDK's 16 px CJK font as the fallback, the same trick
 * caption_font() uses in muse_ui.c: Latin and digits keep the label's own
 * font, Chinese falls through to the CJK font. */
#if CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define HOME_CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define HOME_CJK_FONT muse_font_cjk_16
#endif

static const lv_font_t *home_font(const lv_font_t *plain)
{
#ifdef HOME_CJK_FONT
    static lv_font_t s_copies[4];
    static const lv_font_t *s_plains[4];
    for (int i = 0; i < 4; i++) {
        if (s_plains[i] == plain) {
            return &s_copies[i];
        }
        if (!s_plains[i]) {
            s_plains[i] = plain;
            s_copies[i] = *plain;
            s_copies[i].fallback = &HOME_CJK_FONT;
            return &s_copies[i];
        }
    }
#endif
    return plain;
}

/* A label whose text may be Chinese: chain the CJK fallback font. */
static void set_cjk(lv_obj_t *l, const char *text, const lv_font_t *plain)
{
    lv_obj_set_style_text_font(l, home_font(plain), 0);
    lv_label_set_text(l, text);
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static const char *wmo_text(int code)
{
    switch (code) {
    case 0: return "晴";
    case 1: return "晴间多云";
    case 2: return "多云";
    case 3: return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 56: case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: case 81: return "阵雨";
    case 82: return "暴雨";
    case 85: case 86: return "阵雪";
    case 95: case 96: case 99: return "雷阵雨";
    default: return "--";
    }
}

static const lv_img_dsc_t *wx_icon_for_code(int code)
{
    switch (code) {
    case 0: return &wx_clear;
    case 1: case 2: return &wx_partly;
    case 3: return &wx_cloudy;
    case 45: case 48: return &wx_fog;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82: return &wx_rain;
    case 71: case 73: case 75: case 77:
    case 85: case 86: return &wx_snow;
    case 95: case 96: case 99: return &wx_thunder;
    default: return &wx_cloudy;
    }
}

/* 0 = Sunday. Sakamoto's algorithm: no clock needed for forecast days. */
static int weekday_of(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static const char *const WEEKDAYS[7] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

/* US AQI bands with the familiar Chinese grades. Approximate against
 * China's own AQI, which Open-Meteo does not publish. */
static const char *aqi_band(int aqi, uint32_t *color)
{
    if (aqi <= 50)  { *color = 0x3fb950; return "优"; }
    if (aqi <= 100) { *color = 0xd29922; return "良"; }
    if (aqi <= 150) { *color = 0xdb6d28; return "轻度"; }
    if (aqi <= 200) { *color = 0xcf222e; return "中度"; }
    *color = 0x6e40c9;
    return "重度";
}

/* ------------------------------------------------------------------ */
/* Cities                                                              */
/* ------------------------------------------------------------------ */

/* The settings page picks from this list; the choice is stored in NVS
 * and the fetch task re-points at the new coordinates at once. The
 * configured CONFIG_MUSE_HOME_CITY is only the first-run default. */
typedef struct {
    const char *zh, *en, *lat, *lon;
} home_city_t;

static const home_city_t CITIES[] = {
    { "上海", "Shanghai",   "31.2304", "121.4737" },
    { "北京", "Beijing",    "39.9042", "116.4074" },
    { "天津", "Tianjin",    "39.3434", "117.3616" },
    { "重庆", "Chongqing",  "29.5630", "106.5516" },
    { "广州", "Guangzhou",  "23.1291", "113.2644" },
    { "深圳", "Shenzhen",   "22.5431", "114.0579" },
    { "杭州", "Hangzhou",   "30.2741", "120.1551" },
    { "南京", "Nanjing",    "32.0603", "118.7969" },
    { "苏州", "Suzhou",     "31.2989", "120.5853" },
    { "成都", "Chengdu",    "30.5728", "104.0668" },
    { "武汉", "Wuhan",      "30.5928", "114.3055" },
    { "西安", "Xi'an",      "34.3416", "108.9398" },
    { "长沙", "Changsha",   "28.2282", "112.9388" },
    { "青岛", "Qingdao",    "36.0671", "120.3826" },
    { "大连", "Dalian",     "38.9140", "121.6147" },
    { "厦门", "Xiamen",     "24.4798", "118.0894" },
    { "昆明", "Kunming",    "25.0389", "102.7183" },
    { "哈尔滨", "Harbin",   "45.8038", "126.5349" },
    { "乌鲁木齐", "Urumqi", "43.8256", "87.6168" },
    { "香港", "Hong Kong",  "22.3193", "114.1694" },
};
#define CITY_COUNT (int)(sizeof(CITIES) / sizeof(CITIES[0]))
_Static_assert(CITY_COUNT <= MUSE_HOME_CITY_MAX, "city table too big");

static int s_city_idx = -1;             /* resolved at build */
static volatile bool s_city_changed;    /* settings -> fetch task */

int muse_home_city_count(void)
{
    return CITY_COUNT;
}

const char *muse_home_city_name_en(int i)
{
    return (i < 0 || i >= CITY_COUNT) ? "" : CITIES[i].en;
}

int muse_home_city_index(void)
{
    return s_city_idx < 0 ? 0 : s_city_idx;
}

/* First-run default from the overlay, then whatever NVS remembers. */
static void city_resolve(void)
{
    int idx = 0;
    for (int i = 0; i < CITY_COUNT; i++) {
        if (strcmp(CITIES[i].zh, CONFIG_MUSE_HOME_CITY) == 0) {
            idx = i;
            break;
        }
    }
    nvs_handle_t h;
    if (nvs_open("musehome", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "city", &v) == ESP_OK && v < CITY_COUNT) {
            idx = v;
        }
        nvs_close(h);
    }
    s_city_idx = idx;
}

void muse_home_set_city(int i)
{
    if (i < 0 || i >= CITY_COUNT || i == s_city_idx) {
        return;
    }
    s_city_idx = i;
    nvs_handle_t h;
    if (nvs_open("musehome", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "city", (uint8_t)i);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_city_lbl) {
        set_cjk(s_city_lbl, CITIES[i].zh, &lv_font_montserrat_20);
        layout_status_icons();
    }
    s_city_changed = true;   /* the fetch task wakes and refetches */
    ESP_LOGI(TAG, "city -> %s", CITIES[i].en);
}

/* ------------------------------------------------------------------ */
/* UI updates (LVGL context only)                                      */
/* ------------------------------------------------------------------ */

static void update_clock(void)
{
    time_t now = time(NULL);
    if (now < 1704067200) {   /* before 2024: SNTP has not landed yet */
        if (s_clock_synced || s_last_min != -2) {
            s_clock_synced = false;
            s_last_min = -2;
            lv_label_set_text(s_time_lbl, "--:--");
            set_cjk(s_date_lbl, "等待对时", &lv_font_montserrat_14);
        }
        return;
    }
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (s_clock_synced && tmv.tm_min == s_last_min) {
        return;
    }
    s_clock_synced = true;
    s_last_min = tmv.tm_min;
    char buf[48];
    snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(s_time_lbl, buf);
    snprintf(buf, sizeof buf, "%d月%d日 %s", tmv.tm_mon + 1, tmv.tm_mday,
             WEEKDAYS[tmv.tm_wday]);
    set_cjk(s_date_lbl, buf, &lv_font_montserrat_14);
}

static void update_weather(void)
{
    home_wx_t w;
    xSemaphoreTake(s_wx_lock, portMAX_DELAY);
    w = s_pending;
    s_wx_dirty = false;
    xSemaphoreGive(s_wx_lock);
    s_shown = w;
    if (!w.valid) {
        return;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f°", (double)w.cur_temp);
    lv_label_set_text(s_temp_lbl, buf);
    set_cjk(s_cond_lbl, wmo_text(w.cur_code), &lv_font_montserrat_20);
    lv_image_set_src(s_wx_icon, wx_icon_for_code(w.cur_code));
    lv_obj_remove_flag(s_wx_icon, LV_OBJ_FLAG_HIDDEN);
    snprintf(buf, sizeof buf, "最高 %.0f°  最低 %.0f°",
             (double)w.days[0].tmax, (double)w.days[0].tmin);
    set_cjk(s_hilo_lbl, buf, &lv_font_montserrat_14);
    if (w.humidity >= 0) {
        snprintf(buf, sizeof buf, "湿度 %d%%", w.humidity);
    } else {
        snprintf(buf, sizeof buf, "湿度 --");
    }
    set_cjk(s_hum_lbl, buf, &lv_font_montserrat_14);
    if (w.aqi_valid) {
        uint32_t color;
        const char *band = aqi_band(w.aqi, &color);
        lv_obj_set_style_bg_color(s_aqi_chip, lv_color_hex(color), 0);
        snprintf(buf, sizeof buf, "空气 %s", band);
        set_cjk(s_aqi_lbl, buf, &lv_font_montserrat_14);
        lv_obj_remove_flag(s_aqi_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_aqi_chip, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < WX_DAYS; i++) {
        const home_day_t *d = &w.days[i];
        if (i == 0) {
            set_cjk(s_day_wd[i], "今天", &lv_font_montserrat_14);
        } else {
            set_cjk(s_day_wd[i], WEEKDAYS[weekday_of(d->year, d->mon,
                                                     d->day)],
                    &lv_font_montserrat_14);
        }
        snprintf(buf, sizeof buf, "%d/%d", d->mon, d->day);
        lv_label_set_text(s_day_dt[i], buf);
        set_cjk(s_day_cd[i], wmo_text(d->code), &lv_font_montserrat_14);
        snprintf(buf, sizeof buf, "%.0f/%.0f°", (double)d->tmin,
                 (double)d->tmax);
        lv_label_set_text(s_day_tp[i], buf);
    }
}

static void home_tick(lv_timer_t *timer)
{
    (void)timer;
    update_clock();
    /* Re-run the icon layout here, not just at build: label geometry
     * reads as 0 until LVGL's first refresh, so the build-time layout
     * placed the row off-screen (start=-104); once sizes are real the
     * same call lands it next to the clock, and it also tracks any
     * later clock/city width change. */
    layout_status_icons();
    update_status_icons();
    if (s_wx_dirty) {
        update_weather();
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

/* GETs url into a fresh PSRAM buffer (caller frees), or NULL. */
static char *http_get(const char *url)
{
    http_acc_t acc = {0};
    acc.cap = 24 * 1024;
    acc.buf = heap_caps_malloc(acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!acc.buf) {
        return NULL;
    }
    acc.buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &acc,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(acc.buf);
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || acc.len == 0) {
        ESP_LOGW(TAG, "GET %s -> err %s, HTTP %d, %u bytes", url,
                 esp_err_to_name(err), status, (unsigned)acc.len);
        free(acc.buf);
        return NULL;
    }
    return acc.buf;
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

static bool fetch_weather(home_wx_t *out, const home_city_t *city)
{
    char url[512];
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%s"
             "&longitude=%s&current=temperature_2m,relative_humidity_2m,"
             "weather_code&daily=weather_code,temperature_2m_max,"
             "temperature_2m_min&timezone=Asia%%2FShanghai&forecast_days=4",
             city->lat, city->lon);
    char *body = http_get(url);
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return false;
    }
    bool ok = false;
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    double temp, code;
    if (cur && daily && num_field(cur, "temperature_2m", &temp) &&
        num_field(cur, "weather_code", &code)) {
        double hum;
        out->cur_temp = (float)temp;
        out->cur_code = (int)code;
        out->humidity = num_field(cur, "relative_humidity_2m", &hum)
                        ? (int)hum : -1;
        cJSON *times = cJSON_GetObjectItemCaseSensitive(daily, "time");
        cJSON *codes = cJSON_GetObjectItemCaseSensitive(daily,
                                                        "weather_code");
        cJSON *maxs = cJSON_GetObjectItemCaseSensitive(
            daily, "temperature_2m_max");
        cJSON *mins = cJSON_GetObjectItemCaseSensitive(
            daily, "temperature_2m_min");
        ok = cJSON_IsArray(times) && cJSON_IsArray(codes) &&
             cJSON_IsArray(maxs) && cJSON_IsArray(mins) &&
             cJSON_GetArraySize(times) >= WX_DAYS;
        for (int i = 0; ok && i < WX_DAYS; i++) {
            const char *ts = cJSON_GetArrayItem(times, i)->valuestring;
            cJSON *cd = cJSON_GetArrayItem(codes, i);
            cJSON *mx = cJSON_GetArrayItem(maxs, i);
            cJSON *mn = cJSON_GetArrayItem(mins, i);
            if (!ts || !cJSON_IsNumber(cd) || !cJSON_IsNumber(mx) ||
                !cJSON_IsNumber(mn)) {
                ok = false;
                break;
            }
            home_day_t *d = &out->days[i];
            d->code = cd->valueint;
            d->tmax = (float)mx->valuedouble;
            d->tmin = (float)mn->valuedouble;
            if (sscanf(ts, "%d-%d-%d", &d->year, &d->mon, &d->day) != 3) {
                ok = false;
            }
        }
    }
    cJSON_Delete(root);
    if (ok) {
        out->valid = true;
    }
    return ok;
}

static void fetch_aqi(home_wx_t *out, const home_city_t *city)
{
    char url[320];
    snprintf(url, sizeof url,
             "https://air-quality-api.open-meteo.com/v1/air-quality"
             "?latitude=%s&longitude=%s&current=us_aqi",
             city->lat, city->lon);
    char *body = http_get(url);
    if (!body) {
        return;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return;
    }
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    double aqi;
    if (cur && num_field(cur, "us_aqi", &aqi)) {
        out->aqi = (int)aqi;
        out->aqi_valid = true;
    }
    cJSON_Delete(root);
}

static void wx_task(void *arg)
{
    (void)arg;
    /* Nothing else on the gadget sets the clock unless an app asks;
     * the home page needs it, so start SNTP here. Already running is
     * fine (the apps layer may have beaten us to it). */
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
    }
    /* Boot has the VM lookup, the Noise connect and the chat
     * connect all doing TLS in the first ~10 s; a weather TLS in
     * the same window can fail to even set up (mbedtls -0x008D).
     * The page shows placeholders until data lands anyway, so
     * hold the first fetch until the herd has passed. */
    vTaskDelay(pdMS_TO_TICKS(8000));
    for (;;) {
        if (!muse_wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        const home_city_t *city = &CITIES[muse_home_city_index()];
        home_wx_t w = {0};
        bool ok = fetch_weather(&w, city);
        if (ok) {
            fetch_aqi(&w, city);
            xSemaphoreTake(s_wx_lock, portMAX_DELAY);
            s_pending = w;
            s_wx_dirty = true;
            xSemaphoreGive(s_wx_lock);
            ESP_LOGI(TAG, "weather: %.1fC code %d hum %d, AQI %d",
                     (double)w.cur_temp, w.cur_code, w.humidity,
                     w.aqi_valid ? w.aqi : -1);
        }
        /* Sleep in slices so a city change refetches at once. */
        int wait_s = ok ? CONFIG_MUSE_HOME_REFRESH_MIN * 60 : 2 * 60;
        for (int t = 0; t < wait_s && !s_city_changed; t++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        s_city_changed = false;
    }
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

void muse_home_build(lv_obj_t *tile)
{
    city_resolve();
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar: city and date on the left, the clock on the right.
     * City/date y are initial values; the 1 s tick centers the city
     * on the clock's centerline (see layout_status_icons). */
    s_city_lbl = mk_label(tile, &lv_font_montserrat_20, COL_TEXT, 12, 11);
    set_cjk(s_city_lbl, CITIES[s_city_idx].zh, &lv_font_montserrat_20);
    s_date_lbl = mk_label(tile, &lv_font_montserrat_14, COL_DIM, 12, 38);
    set_cjk(s_date_lbl, "等待对时", &lv_font_montserrat_14);
    s_time_lbl = mk_label(tile, &lv_font_montserrat_28, COL_TEXT, 0, 8);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_RIGHT, -12, 8);

    /* Status icons, left of the clock. */
    for (int i = 0; i < STAT_N; i++) {
        s_stat[i] = lv_image_create(tile);
        lv_obj_remove_flag(s_stat[i],
                           LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_image_set_src(s_stat[0], &stat_wifi_faint);
    lv_image_set_src(s_stat[1], &stat_ble_faint);
    lv_image_set_src(s_stat[2], &stat_spk_lit);
    lv_image_set_src(s_stat[3], &stat_mic_faint);
    lv_image_set_src(s_stat[4], &stat_cam_faint);
    lv_image_set_src(s_stat[5], &stat_sd_faint);
    layout_status_icons();
    update_status_icons();

    lv_obj_t *line = mk_panel(tile, 12, 58, 296, 1, COL_LINE);
    lv_obj_set_style_radius(line, 0, 0);

    /* Main panel: current conditions. The colorful condition icon
     * sits left of the temperature (36 px, centered on the temp row);
     * it stays hidden until the first valid fetch. */
    lv_obj_t *main_panel = mk_panel(tile, 8, 66, 304, 84, COL_PANEL);
    s_wx_icon = lv_image_create(main_panel);
    lv_obj_remove_flag(s_wx_icon,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_wx_icon, 12, 8);
    lv_image_set_src(s_wx_icon, &wx_cloudy);
    lv_obj_add_flag(s_wx_icon, LV_OBJ_FLAG_HIDDEN);
    s_temp_lbl = mk_label(main_panel, &lv_font_montserrat_28, COL_TEXT,
                          56, 10);
    lv_label_set_text(s_temp_lbl, "--°");
    s_cond_lbl = mk_label(main_panel, &lv_font_montserrat_20, COL_TEXT,
                          140, 12);
    set_cjk(s_cond_lbl, "加载中", &lv_font_montserrat_20);
    s_hilo_lbl = mk_label(main_panel, &lv_font_montserrat_14, COL_DIM,
                          140, 44);
    s_hum_lbl = mk_label(main_panel, &lv_font_montserrat_14, COL_DIM,
                         14, 52);
    s_aqi_chip = mk_panel(main_panel, 216, 10, 76, 28, 0x3fb950);
    lv_obj_set_style_radius(s_aqi_chip, 6, 0);
    s_aqi_lbl = lv_label_create(s_aqi_chip);
    lv_obj_set_style_text_color(s_aqi_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_aqi_lbl);
    lv_obj_add_flag(s_aqi_chip, LV_OBJ_FLAG_HIDDEN);

    /* Forecast row. */
    for (int i = 0; i < WX_DAYS; i++) {
        lv_obj_t *p = mk_panel(tile, 8 + i * 103, 158, 98, 74, COL_PANEL2);
        s_day_wd[i] = mk_label(p, &lv_font_montserrat_14, COL_TEXT, 0, 6);
        lv_obj_set_width(s_day_wd[i], 98);
        lv_obj_set_style_text_align(s_day_wd[i], LV_TEXT_ALIGN_CENTER, 0);
        s_day_dt[i] = mk_label(p, &lv_font_montserrat_14, COL_DIM, 0, 24);
        lv_obj_set_width(s_day_dt[i], 98);
        lv_obj_set_style_text_align(s_day_dt[i], LV_TEXT_ALIGN_CENTER, 0);
        s_day_cd[i] = mk_label(p, &lv_font_montserrat_14, COL_TEXT, 0, 42);
        lv_obj_set_width(s_day_cd[i], 98);
        lv_obj_set_style_text_align(s_day_cd[i], LV_TEXT_ALIGN_CENTER, 0);
        s_day_tp[i] = mk_label(p, &lv_font_montserrat_14, COL_TEXT, 0, 58);
        lv_obj_set_width(s_day_tp[i], 98);
        lv_obj_set_style_text_align(s_day_tp[i], LV_TEXT_ALIGN_CENTER, 0);
        set_cjk(s_day_wd[i], i == 0 ? "今天" : "--", &lv_font_montserrat_14);
        set_cjk(s_day_cd[i], "--", &lv_font_montserrat_14);
    }

    s_wx_lock = xSemaphoreCreateMutex();
    lv_timer_create(home_tick, 1000, NULL);
    xTaskCreateWithCaps(wx_task, "home_wx", 10240, NULL, 5, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "home page built for %s", CITIES[s_city_idx].en);
}
