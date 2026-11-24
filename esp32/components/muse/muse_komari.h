#pragma once
/* Komari page (CONFIG_MUSE_KOMARI_PAGE): a tile between the home page
 * and the Muse face that rotates through the servers of a Komari
 * monitor panel (online state, CPU / memory / disk usage, network
 * rates, uptime, days to expiry). The panel base URL and the rotation
 * interval are set from the settings tile and persisted in NVS. */
#include <stddef.h>

#include "lvgl.h"

void muse_komari_build(lv_obj_t *tile);

const char *muse_komari_url(void);
void muse_komari_set_url(const char *url);
int muse_komari_rotate_s(void);
void muse_komari_set_rotate_s(int secs);
int muse_komari_node_count(void);
/* Short human status for the settings pages, e.g. "11 nodes, updated
 * 23:41", "Not fetched yet" or "No panel set". */
void muse_komari_status(char *buf, size_t len);
