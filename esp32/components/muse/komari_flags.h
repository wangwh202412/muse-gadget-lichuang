#pragma once
/* Country flags for the Komari page: pre-rendered 36x24 RGB565 images
 * for the commonly used VPS countries. komari_flag() takes an ISO
 * alpha-2 code ("JP") and returns NULL when no flag is bundled, so the
 * caller can fall back to showing the code as text. */
#include "lvgl.h"

const lv_image_dsc_t *komari_flag(const char *code);
