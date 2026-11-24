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

#pragma once

#include <stddef.h>

#include "lvgl.h"

/*
 * A Xiaozhi (xiaozhi-esp32 protocol) voice-agent tile, sitting between
 * the Komari tile and the Muse face. While this tile is showing, the
 * talk button is diverted from Muse (muse_input_set_ptt_hook) and
 * toggles a Xiaozhi conversation instead: press once to connect and
 * talk (the server endpoints with its own VAD), press again to hang
 * up. Sliding away from the tile also ends the session.
 *
 * The backend comes from an OTA URL: the official Xiaozhi service by
 * default (CONFIG_MUSE_XIAOZHI_OTA_URL), or a self-hosted
 * xiaozhi-esp32-server. Settings -> Xiaozhi changes it; the choice is
 * stored in NVS. Protocol version 1 over WebSocket, Opus 16 kHz mono.
 */

/* Build the tile's contents; called once from muse_ui. */
void muse_xiaozhi_build(lv_obj_t *tile);

/* The OTA URL in effect (NVS choice, else the Kconfig default). */
const char *muse_xiaozhi_ota_url(void);
void muse_xiaozhi_set_ota_url(const char *url);

/* One-line state summary for the Settings page. */
void muse_xiaozhi_status(char *buf, size_t len);
