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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Captures one frame: raw JPEG bytes in PSRAM, caller frees with
 * heap_caps_free(). The Xiaozhi vision upload takes the raw bytes;
 * photo notes and camera.capture take the base64 form instead. */
bool lichuang_camera_capture_jpeg(uint8_t **out_jpeg, size_t *out_len,
                                  const char **error);
/* Captures one frame for a photo note: a malloc'd base64 JPEG. */
bool lichuang_camera_capture(char **jpeg_base64, const char **error);
/* Nothing to close on this board; photo notes call it after a capture. */
void lichuang_camera_done(void);

#ifdef __cplusplus
}
#endif
