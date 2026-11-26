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
 * GC2145 DVP camera on the lichuang-dev board, through the esp32-camera
 * component: one JPEG per capture for photo notes (muse_photo_note.c),
 * the camera.capture Home Link command (main/app.c) and the Xiaozhi MCP
 * camera tool (muse_xiaozhi.c). The pin map matches the board schematic
 * and the xiaozhi lichuang-dev board config, which runs this camera on
 * the same wiring. SCCB shares the board's I2C bus (port 0, created by
 * board_lichuang_dev.c for the audio codecs): pin_sccb_sda = -1 selects
 * that shared-bus mode. Camera power (PCA9557 P2, net DVP_PWDN) is
 * already driven low by the board's expander setup at boot, so there is
 * no power step here. The sensor is initialised once, on the first
 * capture, and deliberately never de-initialised afterwards:
 * esp_camera_deinit() races the still-streaming GDMA (ll_cam_stop only
 * sets link.stop and never waits for the in-flight transfer to finish),
 * and on 2026-11-25 the second capture's deinit double-freed inside
 * cam_deinit (tlsf assert, backtrace-decoded from the ELF) and rebooted
 * the board. While initialised the driver holds the LCD_CAM DMA channel,
 * ~2 KB of DMA descriptors and the PSRAM frame buffer; the frame buffer
 * is PSRAM, not DMA, so the Link re-register DMA gate is not threatened
 * by keeping it (heartbeats show the link works with the largest DMA
 * block at 4-13 KB anyway). One capture at a time: the driver is not thread-safe, and the three callers can overlap, so
 * a collision returns "camera busy" and nobody waits.
 *
 * Sensor, corrected 2026-11-25: the silicon on this board is a GC2145
 * (PID 0x2145, read from the hardware), not the GC0308 the board config
 * suggested. GC2145 has no JPEG encoder: esp_camera_init with
 * PIXFORMAT_JPEG fails with ESP_ERR_NOT_SUPPORTED. Frames are captured
 * as RGB565 and software-encoded to JPEG with esp_new_jpeg (SIMD on
 * the S3), one shot per capture, in PSRAM.
 *
 * Resolution, changed 2026-11-26: QVGA (320x240), not VGA. The VGA
 * 1/2-subsample readout (1280x960 window) produced truncated frames
 * most of the time — the driver dropped ~100 frames in ~8 s and only
 * 1 of 4 Xiaozhi captures in a 120 s log got a complete frame, even
 * with a sensor software reset between attempts. QVGA is the 1/3
 * subsample (960x720 window), half the data rate, and is what the
 * stock Xiaozhi firmware and the SZPI reference use on this sensor.
 *
 * DMA, found 2026-11-25: with the bounce buffer the driver needs one
 * contiguous DMA block for the frame (30720 bytes at VGA RGB565), but
 * the largest free DMA block at runtime is only ~16 KB, so
 * cam_dma_config fails. PSRAM DMA mode
 * (esp_camera_set_psram_mode(true) before init) makes the LCD_CAM DMA
 * write straight into the PSRAM frame buffer, so no bounce buffer is
 * allocated at all.
 */

#include "lichuang_camera.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "lichuang_camera";

#define CAM_W 320
#define CAM_H 240
#define CAM_RGB_LEN (CAM_W * CAM_H * 2)
#define CAM_JPEG_QUALITY 80
#define CAM_JPEG_OUT_CAP (256 * 1024)

static const camera_config_t s_cam_config = {
    .pin_pwdn = -1,
    .pin_reset = -1,
    .pin_xclk = 5,
    .pin_sccb_sda = -1,
    .pin_sccb_scl = 2,
    .pin_d7 = 9,
    .pin_d6 = 4,
    .pin_d5 = 6,
    .pin_d4 = 15,
    .pin_d3 = 17,
    .pin_d2 = 8,
    .pin_d1 = 18,
    .pin_d0 = 16,
    .pin_vsync = 3,
    .pin_href = 46,
    .pin_pclk = 7,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_2,
    .ledc_channel = LEDC_CHANNEL_2,
    .sccb_i2c_port = 0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 0,   /* unused: RGB565 frames, JPEG is software-encoded */
    /* Two buffers + GRAB_LATEST, changed 2026-11-26: with fb_count=1 and
     * GRAB_WHEN_EMPTY every capture returned the frame that was captured
     * right after the PREVIOUS capture's fb_return — the camera goes
     * IDLE once its single buffer is queued (driver: cam_start_frame
     * finds no free buffer), so the queue holds a frame from seconds
     * ago and fb_get returns it in ~1 ms. The user saw it as "every
     * second capture repeats the previous image". With two buffers the
     * camera keeps streaming while one frame is queued, and GRAB_LATEST
     * makes the queue always hold the newest frame (driver pops the old
     * one when a new frame completes). Cost: one extra 153600-byte
     * QVGA frame buffer in PSRAM (~2.3 MB free, fits easily). */
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static bool s_inited;
static atomic_bool s_capturing;
static bool s_resync_needed;  /* a capture failed: the sensor may be stuck
                                 * producing short frames; re-sync it before
                                 * the next capture. */

static bool cam_init(void)
{
    if (s_inited) return true;
    /* PSRAM DMA mode, before the first init: the driver's bounce buffer
     * needs one contiguous DMA block larger than the ~16 KB largest free
     * DMA block at runtime, so init fails without it. The call returns
     * ESP_ERR_INVALID_STATE before the first init; that is expected —
     * the mode flag itself is already set and esp_camera_init below
     * picks it up. */
    esp_camera_set_psram_mode(true);
    esp_err_t err = esp_camera_init(&s_cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_inited = true;
    /* Avoid a warm-up capture here. Some esp32-camera builds report an
     * incomplete PSRAM frame during startup; returning that frame and then
     * immediately capturing again can trip driver buffer ownership. Let the
     * caller perform the first capture and validate its dimensions instead. */
    return true;
}

/* Encode one QVGA RGB565 frame to JPEG. The S3 encoder needs a 16-byte
 * aligned input buffer; the camera driver's frame buffer is not
 * guaranteed to be one, so copy into an aligned buffer when it is not.
 * Returns the JPEG length, or 0 on failure (error set). */
static size_t encode_jpeg(const uint8_t *rgb, size_t rgb_len,
                          uint8_t *out, size_t out_cap, const char **error)
{
    const uint8_t *in = rgb;
    uint8_t *aligned = NULL;
    if (((uintptr_t)rgb & 15) != 0) {
        aligned = (uint8_t *)jpeg_calloc_align(rgb_len, 16);
        if (!aligned) {
            if (error) *error = "out of memory";
            return 0;
        }
        memcpy(aligned, rgb, rgb_len);
        in = aligned;
    }
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = CAM_W;
    cfg.height = CAM_H;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = CAM_JPEG_QUALITY;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.task_enable = false;
    jpeg_enc_handle_t enc = NULL;
    jpeg_error_t err = jpeg_enc_open(&cfg, &enc);
    if (err != JPEG_ERR_OK || enc == NULL) {
        if (error) *error = "jpeg encoder open failed";
        if (aligned) jpeg_free_align(aligned);
        return 0;
    }
    int out_len = 0;
    err = jpeg_enc_process(enc, in, (int)rgb_len, out, (int)out_cap, &out_len);
    jpeg_enc_close(enc);
    if (aligned) jpeg_free_align(aligned);
    if (err != JPEG_ERR_OK || out_len <= 0) {
        if (error) *error = "jpeg encode failed";
        return 0;
    }
    return (size_t)out_len;
}

/* Re-sync a sensor stuck producing short (incomplete) frames. The driver
 * drops every frame whose DMA descriptors are not all filled, so
 * esp_camera_fb_get() times out forever while the sensor keeps streaming
 * truncated frames (seen 2026-11-26: ~100 frames dropped in ~8 s, both
 * photo-note attempts, while Xiaozhi captures on the same build worked).
 * A software reset clears all sensor registers to their defaults and
 * reloads the init table; re-applying pixformat + framesize then puts it
 * back into our RGB565 QVGA mode. This touches only the sensor (SCCB
 * registers) — no deinit, no DMA teardown — so the GDMA double-free that
 * killed the 2026-11-25 build cannot happen here. */
static bool sensor_resync(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->reset || !s->set_pixformat || !s->set_framesize) {
        ESP_LOGW(TAG, "resync: sensor API unavailable");
        return false;
    }
    if (s->reset(s) != 0) {
        ESP_LOGW(TAG, "resync: sensor reset failed");
        return false;
    }
    if (s->set_pixformat(s, PIXFORMAT_RGB565) != 0) {
        ESP_LOGW(TAG, "resync: set_pixformat failed");
        return false;
    }
    if (s->set_framesize(s, FRAMESIZE_QVGA) != 0) {
        ESP_LOGW(TAG, "resync: set_framesize failed");
        return false;
    }
    /* Let the sensor start streaming in its new mode before the caller
     * waits for a frame: the first frame after a re-apply is often
     * incomplete while AE/timing settles. */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "resync: sensor reset + RGB565 QVGA re-applied");
    return true;
}

bool lichuang_camera_capture_jpeg(uint8_t **out_jpeg, size_t *out_len,
                                  const char **error)
{
    *out_jpeg = NULL;
    if (out_len) *out_len = 0;
    if (error) *error = NULL;
    if (atomic_exchange(&s_capturing, true)) {
        if (error) *error = "camera busy";
        return false;
    }
    bool ok = false;
    if (!cam_init()) {
        if (error) *error = "camera init failed";
        goto out;
    }
    camera_fb_t *fb = NULL;
    if (s_resync_needed) {
        ESP_LOGI(TAG, "capture: re-syncing sensor (previous capture failed)");
        sensor_resync();
        s_resync_needed = false;
    }
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        ESP_LOGI(TAG, "capture: waiting for QVGA frame (attempt %u/2)", attempt);
        fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGI(TAG, "capture: frame acquired on attempt %u", attempt);
            break;
        }
        ESP_LOGW(TAG, "capture: no frame on attempt %u/2; %s",
                 attempt, attempt == 1 ? "re-syncing sensor and retrying" : "giving up");
        if (attempt == 1) {
            /* The sensor is streaming truncated frames (the driver drops
             * every one as incomplete). Re-sync it before the last try. */
            sensor_resync();
        }
    }
    if (!fb) {
        ESP_LOGE(TAG, "capture: esp_camera_fb_get returned NULL after 2 attempts");
        s_resync_needed = true;   /* next capture re-syncs the sensor first */
        if (error) *error = "camera capture failed";
        goto out;
    }
    ESP_LOGI(TAG, "capture: frame %ux%u format=%d len=%u",
             (unsigned)fb->width, (unsigned)fb->height, (int)fb->format,
             (unsigned)fb->len);
    if (fb->format == PIXFORMAT_RGB565 && fb->len == CAM_RGB_LEN) {
        /* Copy the frame to a private buffer and byte-swap it there,
         * then return the frame buffer immediately. The GC2145 sends
         * RGB565 big-endian on the DVP bus (high byte first), but the
         * JPEG encoder expects little-endian. Without the swap every
         * dark neutral turns magenta and bright areas go green — exactly
         * the distortion in the 2026-11-25 photo (verified by pixel
         * math: a dark neutral 0x18E3 reads back as bright magenta).
         * The stock Xiaozhi firmware byte-swaps RGB565 by default for
         * the same reason.
         *
         * The copy is load-bearing, not just tidiness: with fb_count=2
         * + GRAB_LATEST (2026-11-26) the camera streams continuously,
         * and Muse photo notes taken after that change came back with
         * the same magenta/green noise — whole frames, or a clean top
         * half over a noisy bottom half with a sharp boundary. That is
         * the signature of the frame buffer being touched while the
         * capture is still working on it (an in-place swap + ~35 ms
         * encode holds the driver's buffer for the whole encode).
         * Copying the frame out (~1 ms), swapping the copy, and giving
         * the buffer back before encoding removes the window entirely:
         * the encoder only ever reads private memory the DMA cannot
         * reach. */
        unsigned w = (unsigned)fb->width;
        unsigned h = (unsigned)fb->height;
        unsigned flen = (unsigned)fb->len;
        uint8_t *rgb = (uint8_t *)heap_caps_aligned_alloc(16, CAM_RGB_LEN,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb) {
            if (error) *error = "out of memory";
        } else {
            const uint16_t *src = (const uint16_t *)fb->buf;
            uint16_t *dst = (uint16_t *)rgb;
            for (size_t i = 0; i < (size_t)CAM_W * CAM_H; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
            /* The frame buffer is ours no longer: hand it back now so
             * the driver can keep streaming, before the ~35 ms encode. */
            ESP_LOGI(TAG, "capture: returning frame buffer");
            esp_camera_fb_return(fb);
            fb = NULL;
            uint8_t *jpg = (uint8_t *)heap_caps_malloc(CAM_JPEG_OUT_CAP,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!jpg) {
                if (error) *error = "out of memory";
            } else {
                int64_t t0 = esp_timer_get_time();
                size_t jpg_len = encode_jpeg(rgb, CAM_RGB_LEN, jpg, CAM_JPEG_OUT_CAP, error);
                if (jpg_len) {
                    *out_jpeg = jpg;
                    if (out_len) *out_len = jpg_len;
                    ok = true;
                    ESP_LOGI(TAG, "photo: %ux%u RGB565 %u bytes -> JPEG %u bytes in %lld ms",
                             w, h, flen, (unsigned)jpg_len,
                             (long long)((esp_timer_get_time() - t0) / 1000));
                } else {
                    heap_caps_free(jpg);
                }
            }
            heap_caps_free(rgb);
        }
    } else {
        if (error) *error = "camera did not return RGB565 QVGA";
    }
    if (fb) {
        ESP_LOGI(TAG, "capture: returning frame buffer");
        esp_camera_fb_return(fb);
        fb = NULL;
    }
    /* Deliberately no esp_camera_deinit() here: deinit races the
     * still-streaming GDMA (ll_cam_stop only sets link.stop and never
     * waits for the in-flight transfer) and double-frees inside
     * cam_deinit on the second capture (2026-11-25, tlsf assert +
     * reboot, backtrace-decoded). The camera stays initialised; it
     * holds only ~2 KB of DMA descriptors and the PSRAM frame buffer
     * while idle. */
out:
    atomic_store(&s_capturing, false);
    return ok;
}

bool lichuang_camera_capture(char **jpeg_base64, const char **error)
{
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (!lichuang_camera_capture_jpeg(&jpg, &jpg_len, error)) return false;
    *jpeg_base64 = NULL;
    bool ok = false;
    size_t cap = 4 * ((jpg_len + 2) / 3) + 1;
    char *b64 = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64) {
        size_t out = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, cap, &out, jpg, jpg_len) == 0) {
            b64[out] = '\0';
            *jpeg_base64 = b64;
            ok = true;
        } else {
            heap_caps_free(b64);
            if (error) *error = "base64 encode failed";
        }
    } else {
        if (error) *error = "out of memory";
    }
    heap_caps_free(jpg);
    return ok;
}

void lichuang_camera_done(void)
{
    /* Nothing to close: the camera stays initialised for the life of
     * the firmware (see the note at the deinit site above — deinit
     * races the live GDMA stream and double-frees on the second
     * capture). */
}
