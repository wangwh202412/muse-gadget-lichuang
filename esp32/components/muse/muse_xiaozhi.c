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
 * The Xiaozhi tile (muse_xiaozhi.h): a second voice agent beside Muse,
 * speaking the xiaozhi-esp32 protocol (WebSocket, version 1) to the
 * backend an OTA endpoint hands out — the official Xiaozhi service by
 * default, or a self-hosted xiaozhi-esp32-server.
 *
 * One session task owns the lifecycle: BOOT (diverted by the input
 * hook while this tile is showing) toggles it. Connecting runs the OTA
 * POST, opens the WebSocket, says hello, and starts listening with the
 * server endpointing (mode "auto"). A capture task streams the mic as
 * 60 ms Opus frames; a player task decodes the server's Opus back to
 * the speaker. Capture pauses while the server is speaking (there is
 * no echo canceller in this build), so the conversation is effectively
 * half-duplex. Leaving the tile ends the session.
 *
 * Audio goes through muse_audio (16 kHz mono PCM), the same public
 * path the Muse voice task uses when it is busy; the voice task is
 * idle-blocked on its queue while a Xiaozhi session runs, because the
 * only way to start one is a BOOT press this page has diverted.
 */

#include "muse_xiaozhi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "muse_audio.h"
#include "muse_input.h"
#include "muse_voice.h"
#include "nvs.h"
#include "opus.h"
#include "sdkconfig.h"

static const char *TAG = "muse_xiaozhi";

#define COL_BG     0x0a141f
#define COL_TEXT   0xe8f1f2
#define COL_DIM    0x9fb3bd
#define COL_GREEN  0x7ee787
#define COL_AMBER  0xffd166
#define COL_RED    0xff7b72
#define COL_GREY   0x5a6b73

#define FRAME_MS      60
#define FRAME_SAMPLES 960   /* 60 ms at 16 kHz */
#define PLAYQ_LEN     32

typedef enum {
    X_IDLE,
    X_CONNECTING,
    X_ACTIVE,
    X_ACTIVATION,
    X_ERROR,
} xz_state_t;

typedef enum { CMD_TOGGLE } cmd_t;

typedef struct {
    uint16_t len;
    uint8_t data[];
} pkt_t;

/* A frame waiting to be written to the transport (session task is
 * the only writer). */
typedef struct {
    uint8_t opcode;
    uint16_t len;
    uint8_t data[];
} txpkt_t;

/* ---------- settings (NVS) ---------- */

static char s_ota_url[160] = CONFIG_MUSE_XIAOZHI_OTA_URL;
static bool s_ota_loaded;
static char s_client_id[40];
static char s_mac[18];

static void load_settings(void)
{
    if (s_ota_loaded) return;
    s_ota_loaded = true;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    nvs_handle_t h;
    if (nvs_open("musexiaozhi", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof s_ota_url;
    if (nvs_get_str(h, "ota", s_ota_url, &n) != ESP_OK) {
        s_ota_url[sizeof s_ota_url - 1] = '\0';
    }
    n = sizeof s_client_id;
    if (nvs_get_str(h, "cid", s_client_id, &n) != ESP_OK ||
        !s_client_id[0]) {
        uint32_t r[4];
        for (int i = 0; i < 4; i++) r[i] = esp_random();
        snprintf(s_client_id, sizeof s_client_id,
                 "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
                 (unsigned long)r[0], (unsigned long)(r[1] & 0xffff),
                 (unsigned long)((r[1] >> 16) & 0x0fff) | 0x4000,
                 (unsigned long)(r[2] & 0x3fff) | 0x8000,
                 (unsigned long)r[3], (unsigned long)(r[0] & 0xffff));
        nvs_set_str(h, "cid", s_client_id);
        nvs_commit(h);
    }
    nvs_close(h);
}

const char *muse_xiaozhi_ota_url(void)
{
    load_settings();
    return s_ota_url;
}

void muse_xiaozhi_set_ota_url(const char *url)
{
    load_settings();
    if (!url || !url[0]) return;
    snprintf(s_ota_url, sizeof s_ota_url, "%s", url);
    nvs_handle_t h;
    if (nvs_open("musexiaozhi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ota", s_ota_url);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- session state (written by the session/WS tasks,
 * read by the LVGL tick; plain buffers, last writer wins) ---------- */

static volatile xz_state_t s_state = X_IDLE;
static volatile bool s_page_active;
static volatile bool s_speaking;      /* server TTS in flight */
static volatile bool s_session_on;    /* capture/player may run */
static char s_status_err[64];         /* last error detail */
static char s_act_code[16];           /* activation code, if any */
static char s_stt_text[256];
static char s_reply_text[512];
static char s_session_id[48];

static QueueHandle_t s_cmdq;
static QueueHandle_t s_playq;
static QueueHandle_t s_txq;
static esp_transport_handle_t s_ws;
static esp_transport_list_handle_t s_ws_list;
static volatile bool s_hello_ok, s_ws_down;

/* Receive reassembly (session task only), in PSRAM. */
#define RX_CHUNK   2048
#define RX_MSG_CAP 16384
static uint8_t *s_rx_chunk;
static uint8_t *s_rx_msg;
static int s_rx_len;
static int s_rx_frame_left;
static bool s_rx_fin, s_rx_mid, s_rx_overflow;
static ws_transport_opcodes_t s_rx_op;
static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static volatile bool s_tts_stop;      /* server said tts stop */
static volatile bool s_cap_done, s_play_done;

/* ---------- fonts (CJK fallback chain, as on the home/Komari tiles) -- */

#if CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define XZ_CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define XZ_CJK_FONT muse_font_cjk_16
#endif

static const lv_font_t *xz_font(const lv_font_t *plain)
{
#ifdef XZ_CJK_FONT
    static lv_font_t s_copies[4];
    static const lv_font_t *s_plains[4];
    for (int i = 0; i < 4; i++) {
        if (s_plains[i] == plain) {
            return &s_copies[i];
        }
        if (!s_plains[i]) {
            s_plains[i] = plain;
            s_copies[i] = *plain;
            s_copies[i].fallback = &XZ_CJK_FONT;
            return &s_copies[i];
        }
    }
#endif
    return plain;
}

static void set_cjk(lv_obj_t *l, const char *text, const lv_font_t *plain)
{
    lv_obj_set_style_text_font(l, xz_font(plain), 0);
    lv_label_set_text(l, text);
}

/* ---------- UI ---------- */

static lv_obj_t *s_tile;
static lv_obj_t *s_dot;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_stt_lbl;
static lv_obj_t *s_reply_lbl;
static lv_obj_t *s_hint_lbl;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static char s_shown_status[64];
static char s_shown_stt[300];
static char s_shown_reply[600];
static char s_shown_hint[64];

static void render(void)
{
    uint32_t dot = COL_GREY;
    const char *status = "未连接";
    switch (s_state) {
    case X_IDLE:       status = "未连接"; dot = COL_GREY; break;
    case X_CONNECTING: status = "连接中..."; dot = COL_AMBER; break;
    case X_ACTIVE:
        status = s_speaking ? "小智说话中" : "对话中，请说话";
        dot = COL_GREEN;
        break;
    case X_ACTIVATION: status = "等待激活绑定"; dot = COL_AMBER; break;
    case X_ERROR:      status = "连接失败"; dot = COL_RED; break;
    }
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(dot), 0);
    if (strcmp(s_shown_status, status)) {
        snprintf(s_shown_status, sizeof s_shown_status, "%s", status);
        set_cjk(s_status_lbl, status, &lv_font_montserrat_14);
    }

    char buf[600];
    if (s_stt_text[0]) {
        snprintf(buf, sizeof buf, "你： %s", s_stt_text);
    } else {
        buf[0] = '\0';
    }
    if (strcmp(s_shown_stt, buf)) {
        snprintf(s_shown_stt, sizeof s_shown_stt, "%s", buf);
        set_cjk(s_stt_lbl, buf, &lv_font_montserrat_14);
    }
    if (s_state == X_ACTIVATION && s_act_code[0]) {
        snprintf(buf, sizeof buf,
                 "激活码： %s\n打开 xiaozhi.me 登录，在控制台输入此码绑定设备",
                 s_act_code);
    } else if (s_state == X_ERROR && s_status_err[0]) {
        snprintf(buf, sizeof buf, "%s", s_status_err);
    } else if (s_reply_text[0]) {
        snprintf(buf, sizeof buf, "小智： %s", s_reply_text);
    } else {
        buf[0] = '\0';
    }
    if (strcmp(s_shown_reply, buf)) {
        snprintf(s_shown_reply, sizeof s_shown_reply, "%s", buf);
        set_cjk(s_reply_lbl, buf, &lv_font_montserrat_14);
    }
    const char *hint = s_state == X_ACTIVE ? "再按 BOOT 结束对话"
                                           : "按 BOOT 开始和小智对话";
    if (strcmp(s_shown_hint, hint)) {
        snprintf(s_shown_hint, sizeof s_shown_hint, "%s", hint);
        set_cjk(s_hint_lbl, hint, &lv_font_montserrat_14);
    }
}

static void xz_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_tile) return;
    lv_obj_t *tv = lv_obj_get_parent(s_tile);
    bool active = tv && lv_tileview_get_tile_active(tv) == s_tile;
    if (active != s_page_active) {
        s_page_active = active;
    }
    /* Leaving the tile mid-session ends it (BOOT belongs to Muse
     * again on the other pages). The session task polls the flag. */
    render();
}

/* ---------- input hook: BOOT is Xiaozhi's while this tile shows --- */

static volatile bool s_divert_up;

static bool xz_ptt_hook(const muse_input_event_t *ev)
{
    if (ev->wake) return false;   /* a wake tap only wakes */
    if (ev->type == MUSE_PTT_UP) {
        bool d = s_divert_up;
        s_divert_up = false;
        return d;
    }
    if (ev->type != MUSE_PTT_DOWN) return false;
    if (!s_page_active || !s_cmdq) return false;
    s_divert_up = true;
    cmd_t c = CMD_TOGGLE;
    xQueueSend(s_cmdq, &c, 0);
    return true;
}

/* ---------- OTA ---------- */

typedef struct {
    char *buf;
    int len;
    int cap;
} http_acc_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    http_acc_t *acc = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && acc && acc->buf) {
        int room = acc->cap - 1 - acc->len;
        int take = e->data_len < room ? e->data_len : room;
        if (take > 0) {
            memcpy(acc->buf + acc->len, e->data, take);
            acc->len += take;
            acc->buf[acc->len] = '\0';
        }
    }
    return ESP_OK;
}

static char s_ws_url[256];
static char s_ws_token[256];

/* One OTA round. Returns 1 when a websocket endpoint came back, 0 when
 * the answer was an activation challenge, -1 on failure (s_status_err
 * says why). */
static int ota_fetch(void)
{
    s_ws_url[0] = s_ws_token[0] = 0;
    char body[640];
    snprintf(body, sizeof body,
             "{\"version\":2,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"application\":{\"name\":\"muse-xiaozhi\","
             "\"version\":\"1.0.0\"},"
             "\"ota\":{\"label\":\"muse-xiaozhi\"},"
             "\"board\":{\"type\":\"lichuang-dev\","
             "\"name\":\"Lichuang Szpi ESP32-S3\",\"mac\":\"%s\"},"
             "\"chip_model_name\":\"esp32s3\",\"flash_size\":16777216}",
             s_mac, s_client_id, s_mac);
    char *resp = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!resp) return -1;
    http_acc_t acc = { .buf = resp, .len = 0, .cap = 8192 };
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = s_ota_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp); return -1; }
    esp_http_client_set_header(client, "Device-Id", s_mac);
    esp_http_client_set_header(client, "Client-Id", s_client_id);
    esp_http_client_set_header(client, "User-Agent", "lichuang-dev/1.0.0");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        snprintf(s_status_err, sizeof s_status_err,
                 "OTA 请求失败 (HTTP %d)", status);
        free(resp);
        return -1;
    }
    int rc = -1;
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "activation");
        cJSON *code = act ? cJSON_GetObjectItemCaseSensitive(act, "code")
                          : NULL;
        if (cJSON_IsString(code) && code->valuestring) {
            snprintf(s_act_code, sizeof s_act_code, "%s",
                     code->valuestring);
        }
        cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
        cJSON *url = ws ? cJSON_GetObjectItemCaseSensitive(ws, "url") : NULL;
        cJSON *tok = ws ? cJSON_GetObjectItemCaseSensitive(ws, "token")
                        : NULL;
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            snprintf(s_ws_url, sizeof s_ws_url, "%s", url->valuestring);
            if (cJSON_IsString(tok) && tok->valuestring) {
                snprintf(s_ws_token, sizeof s_ws_token, "%s",
                         tok->valuestring);
            }
            rc = 1;
        } else if (s_act_code[0]) {
            rc = 0;
        } else {
            snprintf(s_status_err, sizeof s_status_err,
                     "OTA 回应没有 websocket 地址");
        }
        cJSON_Delete(root);
    } else {
        snprintf(s_status_err, sizeof s_status_err, "OTA 回应无法解析");
    }
    free(resp);
    return rc;
}

/* ---------- WebSocket ---------- */

/* The session task is the only transport writer; other tasks
 * hand frames over through s_txq and the session loop drains it. */
static void ws_write(uint8_t opcode, const void *data, int len)
{
    if (s_ws) {
        /* FIN must ride along: send_raw does not add it, and a frame
         * without FIN leaves the peer waiting for the rest of the
         * message forever. */
        int w = esp_transport_ws_send_raw(
            s_ws,
            (ws_transport_opcodes_t)(opcode | WS_TRANSPORT_OPCODES_FIN),
            (const char *)data, len, 2000);
        if (w < 0) ESP_LOGW(TAG, "ws send op %d len %d -> %d",
                            opcode, len, w);
    }
}

static void ws_send_text(const char *text)
{
    ws_write(WS_TRANSPORT_OPCODES_TEXT, text, (int)strlen(text));
}

static void ws_queue_send(uint8_t opcode, const void *data, int len)
{
    txpkt_t *tx = heap_caps_malloc(sizeof(txpkt_t) + len,
                                   MALLOC_CAP_SPIRAM);
    if (!tx) return;
    tx->opcode = opcode;
    tx->len = (uint16_t)len;
    memcpy(tx->data, data, len);
    if (xQueueSend(s_txq, &tx, 0) != pdTRUE) free(tx);
}

static void handle_mcp(cJSON *payload)
{
    if (!payload) return;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
    if (!id || !cJSON_IsString(method)) return;   /* a notification */
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, 1));
    if (!strcmp(method->valuestring, "initialize")) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON *caps = cJSON_CreateObject();
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "name", "muse-xiaozhi");
        cJSON_AddStringToObject(info, "version", "1.0.0");
        cJSON_AddItemToObject(result, "serverInfo", info);
        cJSON_AddItemToObject(reply, "result", result);
    } else if (!strcmp(method->valuestring, "tools/list")) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", cJSON_CreateArray());
        cJSON_AddItemToObject(reply, "result", result);
    } else {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "method not found");
        cJSON_AddItemToObject(reply, "error", error);
    }
    cJSON *wrap = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap, "session_id", s_session_id);
    cJSON_AddStringToObject(wrap, "type", "mcp");
    cJSON_AddItemToObject(wrap, "payload", reply);
    char *text = cJSON_PrintUnformatted(wrap);
    if (text) {
        ws_send_text(text);
        free(text);
    }
    cJSON_Delete(wrap);
}

static void handle_text(const char *data, int len)
{
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!copy) return;
    memcpy(copy, data, len);
    copy[len] = '\0';
    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        const char *t = type->valuestring;
        {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
            ESP_LOGI(TAG, "srv: %s%s%s", t,
                     cJSON_IsString(st) ? "/" : "",
                     cJSON_IsString(st) ? st->valuestring : "");
        }
        if (!strcmp(t, "hello")) {
            cJSON *sid = cJSON_GetObjectItemCaseSensitive(root,
                                                          "session_id");
            if (cJSON_IsString(sid)) {
                snprintf(s_session_id, sizeof s_session_id, "%s",
                         sid->valuestring);
            }
            s_hello_ok = true;
        } else if (!strcmp(t, "stt")) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(text)) {
                snprintf(s_stt_text, sizeof s_stt_text, "%s",
                         text->valuestring);
            }
        } else if (!strcmp(t, "tts")) {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(st)) {
                if (!strcmp(st->valuestring, "start")) {
                    s_speaking = true;
                    s_tts_stop = false;
                } else if (!strcmp(st->valuestring, "sentence_start")) {
                    if (cJSON_IsString(text)) {
                        snprintf(s_reply_text, sizeof s_reply_text, "%s",
                                 text->valuestring);
                    }
                } else if (!strcmp(st->valuestring, "stop")) {
                    s_tts_stop = true;   /* player clears s_speaking */
                }
            }
        } else if (!strcmp(t, "goodbye")) {
            s_ws_down = true;
        } else if (!strcmp(t, "mcp")) {
            handle_mcp(cJSON_GetObjectItemCaseSensitive(root, "payload"));
        }
    }
    cJSON_Delete(root);
}

static void ws_dispatch(ws_transport_opcodes_t op, const uint8_t *data,
                        int len)
{
    if (op == WS_TRANSPORT_OPCODES_TEXT) {
        handle_text((const char *)data, len);
    } else if (op == WS_TRANSPORT_OPCODES_BINARY && s_session_on) {
        pkt_t *pkt = heap_caps_malloc(sizeof(pkt_t) + len,
                                      MALLOC_CAP_SPIRAM);
        if (pkt) {
            pkt->len = (uint16_t)len;
            memcpy(pkt->data, data, len);
            if (xQueueSend(s_playq, &pkt, 0) != pdTRUE) free(pkt);
        }
    } else if (op == WS_TRANSPORT_OPCODES_CLOSE) {
        s_ws_down = true;
    }
}

/* One poll/read step (session task only); false when the connection
 * is dead. Fragmented messages are reassembled in PSRAM and
 * dispatched whole. */
static bool ws_pump(int timeout_ms)
{
    int ready = esp_transport_poll_read(s_ws, timeout_ms);
    if (ready < 0) { s_ws_down = true; return false; }
    if (ready == 0) return true;
    int n = esp_transport_read(s_ws, (char *)s_rx_chunk, RX_CHUNK,
                               timeout_ms);
    if (n < 0) { s_ws_down = true; return false; }
    if (n == 0) return true;
    if (s_rx_frame_left == 0) {
        int pl = esp_transport_ws_get_read_payload_len(s_ws);
        s_rx_frame_left = pl > 0 ? pl : n;
        if (!s_rx_mid) {
            s_rx_op = esp_transport_ws_get_read_opcode(s_ws);
        }
        s_rx_fin = esp_transport_ws_get_fin_flag(s_ws);
    }
    if (s_rx_len + n <= RX_MSG_CAP) {
        memcpy(s_rx_msg + s_rx_len, s_rx_chunk, n);
        s_rx_len += n;
    } else {
        s_rx_overflow = true;
    }
    s_rx_frame_left -= n;
    if (s_rx_frame_left <= 0) {
        s_rx_frame_left = 0;
        if (s_rx_fin) {
            if (!s_rx_overflow) ws_dispatch(s_rx_op, s_rx_msg, s_rx_len);
            s_rx_len = 0;
            s_rx_overflow = false;
            s_rx_mid = false;
        } else {
            s_rx_mid = true;
        }
    }
    return true;
}

/* ---------- audio tasks ---------- */

static void capture_task(void *arg)
{
    (void)arg;
    int16_t *pcm = heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM);
    uint8_t *out = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    int read_errors = 0;
    int frames = 0;
    while (s_session_on) {
        if (s_speaking) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        size_t got = 0;
        bool ok = true;
        while (got < FRAME_SAMPLES) {
            if (muse_audio_read(pcm + got, MUSE_AUDIO_CHUNK) != ESP_OK) {
                ok = false;
                break;
            }
            got += MUSE_AUDIO_CHUNK;
        }
        if (!ok) {
            if (read_errors == 0 && s_session_on) {
                ESP_LOGW(TAG, "mic read failing (resting=%d)",
                         (int)muse_voice_resting());
            }
            if (++read_errors > 20) break;   /* audio went away */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        read_errors = 0;
        ++frames;
        if (frames % 100 == 0) {
            ESP_LOGI(TAG, "cap %d lvl %.1f dBFS", frames,
                     muse_audio_dbfs(pcm, FRAME_SAMPLES));
        }
        if (frames == 100) {
            /* One datapoint on real stack appetite (words free). */
            ESP_LOGI(TAG, "cap stack hwm: %u bytes free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (!s_enc || !s_ws) break;
        int n = opus_encode(s_enc, pcm, FRAME_SAMPLES, out, 512);
        if (n > 0) ws_queue_send(WS_TRANSPORT_OPCODES_BINARY, out, n);
    }
    free(pcm);
    free(out);
    s_cap_done = true;
    vTaskDelete(NULL);
}

static void player_task(void *arg)
{
    (void)arg;
    int16_t *pcm = heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM);
    while (s_session_on) {
        pkt_t *pkt = NULL;
        if (xQueueReceive(s_playq, &pkt, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_tts_stop &&
                uxQueueMessagesWaiting(s_playq) == 0) {
                s_tts_stop = false;
                s_speaking = false;   /* drained: mic opens again */
                ESP_LOGI(TAG, "tts done, mic live");
                /* Re-arm the server for the next turn: it arms
                 * its VAD/ASR on every listen start (the stock
                 * firmware sends one whenever it returns to
                 * listening). Without this, turn two's audio is
                 * streamed to a server that is not listening. */
                char listen[160];
                int ln = snprintf(listen, sizeof listen,
                                  "{\"session_id\":\"%s\","
                                  "\"type\":\"listen\","
                                  "\"state\":\"start\","
                                  "\"mode\":\"auto\"}",
                                  s_session_id);
                if (ln > 0) {
                    ws_queue_send(WS_TRANSPORT_OPCODES_TEXT,
                                  listen, ln);
                }
            }
            continue;
        }
        if (pkt) {
            if (s_dec) {
                int n = opus_decode(s_dec, pkt->data, pkt->len, pcm,
                                    FRAME_SAMPLES, 0);
                if (n > 0) muse_audio_write(pcm, n);
            }
            free(pkt);
        }
    }
    free(pcm);
    s_play_done = true;
    vTaskDelete(NULL);
}

/* ---------- session lifecycle (runs in the session task) ---------- */

static void ws_close(void)
{
    if (s_ws_list) {
        esp_transport_close(s_ws);
        esp_transport_list_destroy(s_ws_list);
        s_ws = NULL;
        s_ws_list = NULL;
    }
    if (s_enc) { opus_encoder_destroy(s_enc); s_enc = NULL; }
    if (s_dec) { opus_decoder_destroy(s_dec); s_dec = NULL; }
    pkt_t *pkt;
    while (s_playq && xQueueReceive(s_playq, &pkt, 0) == pdTRUE) free(pkt);
    txpkt_t *tx;
    while (s_txq && xQueueReceive(s_txq, &tx, 0) == pdTRUE) free(tx);
    s_rx_len = 0;
    s_rx_frame_left = 0;
    s_rx_mid = false;
    s_rx_overflow = false;
}

/* The transport keeps pointers to the path and headers, so both live
 * in static storage. */
static char s_ws_path[192];
static char s_ws_headers[640];

static bool ws_open(void)
{
    const char *p = s_ws_url;
    bool secure;
    if (!strncmp(p, "wss://", 6)) { secure = true; p += 6; }
    else if (!strncmp(p, "ws://", 5)) { secure = false; p += 5; }
    else return false;
    char host[128];
    int port = secure ? 443 : 80;
    const char *slash = strchr(p, '/');
    const char *hend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hend - p));
    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hend - p);
    if (hlen == 0 || hlen >= sizeof host) return false;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (colon) port = atoi(colon + 1);
    snprintf(s_ws_path, sizeof s_ws_path, "%s", slash ? slash : "/");
    int hn = 0;
    if (s_ws_token[0]) {
        hn += snprintf(s_ws_headers + hn, sizeof s_ws_headers - hn,
                       "Authorization: Bearer %s\r\n", s_ws_token);
    }
    snprintf(s_ws_headers + hn, sizeof s_ws_headers - hn,
             "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             s_mac, s_client_id);

    esp_transport_handle_t base;
    if (secure) {
        base = esp_transport_ssl_init();
        if (base) {
            esp_transport_ssl_crt_bundle_attach(base,
                                                esp_crt_bundle_attach);
        }
    } else {
        base = esp_transport_tcp_init();
    }
    if (!base) return false;
    s_ws = esp_transport_ws_init(base);
    if (!s_ws) { esp_transport_destroy(base); return false; }
    esp_transport_ws_config_t wcfg = {
        .ws_path = s_ws_path,
        .headers = s_ws_headers,
    };
    esp_transport_ws_set_config(s_ws, &wcfg);
    s_ws_list = esp_transport_list_init();
    if (s_ws_list) esp_transport_list_add(s_ws_list, s_ws, "ws");
    s_hello_ok = false;
    s_ws_down = false;
    s_rx_len = 0;
    s_rx_frame_left = 0;
    s_rx_mid = false;
    s_rx_overflow = false;
    int status = -1;
    if (esp_transport_connect(s_ws, host, port, 15000) == ESP_OK) {
        status = esp_transport_ws_get_upgrade_request_status(s_ws);
    }
    if (status != 101) {
        ESP_LOGW(TAG, "ws connect to %s:%d failed, upgrade status %d",
                 host, port, status);
        ws_close();
        return false;
    }
    ESP_LOGI(TAG, "ws up: %s:%d%s", host, port, s_ws_path);
    char hello[320];
    snprintf(hello, sizeof hello,
             "{\"type\":\"hello\",\"version\":1,"
             "\"features\":{\"mcp\":true},"
             "\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\","
             "\"sample_rate\":16000,\"channels\":1,"
             "\"frame_duration\":%d}}", FRAME_MS);
    ws_send_text(hello);
    int64_t deadline = esp_timer_get_time() + 12000000;
    while (!s_hello_ok && !s_ws_down && esp_timer_get_time() < deadline) {
        if (!ws_pump(200)) break;
    }
    if (!s_hello_ok) {
        ESP_LOGW(TAG, "no hello from server (down=%d)", (int)s_ws_down);
        ws_close();
        return false;
    }
    int err = 0;
    s_enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (s_enc) {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }
    s_dec = opus_decoder_create(16000, 1, &err);
    if (!s_enc || !s_dec) {
        ws_close();
        return false;
    }
    char listen[160];
    snprintf(listen, sizeof listen,
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"start\",\"mode\":\"auto\"}", s_session_id);
    ws_send_text(listen);
    return true;
}

/* Runs one whole session; returns when it is over. */
static void run_session(void)
{
    s_status_err[0] = 0;
    s_act_code[0] = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        snprintf(s_status_err, sizeof s_status_err, "Wi-Fi 未连接");
        s_state = X_ERROR;
        return;
    }
    s_state = X_CONNECTING;
    /* OTA rounds: an activation challenge re-polls until bound or the
     * user gives up with BOOT. */
    for (;;) {
        int rc = ota_fetch();
        if (rc == 1) break;
        if (rc == 0) {
            s_state = X_ACTIVATION;
            bool abort = false;
            for (int waited = 0; waited < 120 && !abort; waited += 2) {
                cmd_t c;
                if (xQueueReceive(s_cmdq, &c, pdMS_TO_TICKS(2000)) ==
                    pdTRUE) {
                    abort = true;
                }
            }
            if (abort) {
                s_state = X_IDLE;
                return;
            }
            s_state = X_CONNECTING;
            continue;
        }
        s_state = X_ERROR;
        return;
    }
    if (!ws_open()) {
        if (!s_status_err[0]) {
            snprintf(s_status_err, sizeof s_status_err, "连接服务器失败");
        }
        s_state = X_ERROR;
        return;
    }
    s_state = X_ACTIVE;
    /* The voice loop powers the codecs down while it rests; a
     * Xiaozhi session is invisible to it, so power them up here.
     * Powering off again at the end is gated on the loop still
     * resting then, so its bookkeeping stays true. */
    muse_audio_power(true);
    /* The voice task's idle capture reads the same I2S channel
     * back-to-back, and a second reader starves (every read times
     * out after ~1 s: "channel is not enabled"). Claim sole
     * ownership for the session; released at the end below. */
    muse_voice_set_capture_owner(true);
    /* Probe the capture path: on this board the codec dev can be
     * left with its I2S RX channel disabled even though the codecs
     * report open (reads then fail with "channel is not enabled").
     * A full close/open cycle re-enables it. */
    {
        int16_t probe[MUSE_AUDIO_CHUNK];
        if (muse_audio_read(probe, MUSE_AUDIO_CHUNK) != ESP_OK) {
            ESP_LOGW(TAG, "audio probe failed; reopening codecs");
            muse_audio_power(false);
            muse_audio_power(true);
            if (muse_audio_read(probe, MUSE_AUDIO_CHUNK) != ESP_OK) {
                ESP_LOGW(TAG, "audio probe still failing");
            } else {
                ESP_LOGI(TAG, "audio probe ok after reopen");
            }
        }
    }
    s_session_on = true;
    s_speaking = false;
    s_tts_stop = false;
    s_cap_done = s_play_done = false;
    /* Stacks are generous: libopus encode alone wants several KB,
     * and the capture task only started reaching it once reads
     * began to succeed (an 8 KB stack overflowed on frame one). */
    xTaskCreateWithCaps(capture_task, "xz_cap", 40960, NULL, 5, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(player_task, "xz_play", 16384, NULL, 5, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int inactive_checks = 0;
    for (;;) {
        txpkt_t *tx;
        while (s_txq && xQueueReceive(s_txq, &tx, 0) == pdTRUE) {
            ws_write(tx->opcode, tx->data, tx->len);
            free(tx);
        }
        cmd_t c;
        if (xQueueReceive(s_cmdq, &c, 0) == pdTRUE) {
            break;   /* BOOT again: hang up */
        }
        if (!ws_pump(50) || s_ws_down) break;
        if (!s_page_active) {
            if (++inactive_checks >= 10) break;   /* slid away: end */
        } else {
            inactive_checks = 0;
        }
    }
    s_session_on = false;
    muse_voice_set_capture_owner(false);
    char stop[160];
    snprintf(stop, sizeof stop,
             "{\"session_id\":\"%s\",\"type\":\"listen\","
             "\"state\":\"stop\"}", s_session_id);
    ws_send_text(stop);
    for (int i = 0; i < 50 && !(s_cap_done && s_play_done); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Power down only once the audio tasks have stopped reading:
     * powering off first fails their in-flight read against the
     * disabled channel (one spurious error line per session). */
    if (muse_voice_resting()) muse_audio_power(false);
    ws_close();
    s_speaking = false;
    s_state = X_IDLE;
}

static void session_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmdq, &c, portMAX_DELAY) == pdTRUE) {
            if (s_state == X_IDLE || s_state == X_ERROR) {
                run_session();
                ESP_LOGI(TAG, "sess stack hwm: %u bytes free",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
}

/* ---------- build ---------- */

void muse_xiaozhi_build(lv_obj_t *tile)
{
    load_settings();
    s_tile = tile;
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *content = lv_obj_create(tile);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 320, 240);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    /* Presses must reach the tile, where the UI's volume-swipe
     * handler lives; this page has no tap controls of its own. */
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

    set_cjk(mk_label(content, &lv_font_montserrat_20, COL_TEXT, 12, 10),
            "小智", &lv_font_montserrat_20);

    s_dot = lv_obj_create(content);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_pos(s_dot, 12, 46);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_GREY), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot, 5, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_SCROLLABLE);
    s_status_lbl = mk_label(content, &lv_font_montserrat_14, COL_DIM,
                            28, 42);

    s_stt_lbl = mk_label(content, &lv_font_montserrat_14, COL_TEXT,
                         12, 70);
    lv_obj_set_width(s_stt_lbl, 296);
    lv_obj_set_height(s_stt_lbl, 36);
    lv_label_set_long_mode(s_stt_lbl, LV_LABEL_LONG_MODE_WRAP);
    s_reply_lbl = mk_label(content, &lv_font_montserrat_14, COL_TEXT,
                           12, 110);
    lv_obj_set_width(s_reply_lbl, 296);
    lv_obj_set_height(s_reply_lbl, 80);
    lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
    s_hint_lbl = mk_label(content, &lv_font_montserrat_14, COL_DIM,
                          12, 206);

    s_cmdq = xQueueCreate(4, sizeof(cmd_t));
    s_playq = xQueueCreate(PLAYQ_LEN, sizeof(pkt_t *));
    s_txq = xQueueCreate(16, sizeof(txpkt_t *));
    s_rx_chunk = heap_caps_malloc(RX_CHUNK, MALLOC_CAP_SPIRAM);
    s_rx_msg = heap_caps_malloc(RX_MSG_CAP, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(session_task, "xz_sess", 24576, NULL, 4, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    muse_input_set_ptt_hook(xz_ptt_hook);
    render();
    lv_timer_create(xz_tick, 250, NULL);
}

void muse_xiaozhi_status(char *buf, size_t len)
{
    const char *st = "idle";
    switch (s_state) {
    case X_IDLE: st = "idle"; break;
    case X_CONNECTING: st = "connecting"; break;
    case X_ACTIVE: st = "in conversation"; break;
    case X_ACTIVATION: st = "activation needed"; break;
    case X_ERROR: st = "error"; break;
    }
    snprintf(buf, len, "%s", st);
}
