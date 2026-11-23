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

#include "lvgl.h"

/*
 * The home page: a tileview tile with the clock, the date and the local
 * weather (Open-Meteo, no key; air quality from its US AQI). Built by
 * muse_ui into its own tile; it owns an SNTP sync (nothing else on the
 * gadget sets the clock) and a small fetch task. All LVGL work happens
 * in build and in its 1 s timer; the fetch task only fills a struct.
 */
void muse_home_build(lv_obj_t *tile);

/* The weather city, picked on the settings City page and kept in NVS.
 * Names here are English for the settings UI (its fonts have no CJK);
 * the home page itself shows the Chinese name. */
#define MUSE_HOME_CITY_MAX 24
int muse_home_city_count(void);
const char *muse_home_city_name_en(int i);
int muse_home_city_index(void);
void muse_home_set_city(int i);
