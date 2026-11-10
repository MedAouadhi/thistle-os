/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Maps app's overlays: the GeoJSON files on the card
 */
#pragma once

#include "map_engine/map_view.h"
#include "lvgl.h"

/* Makes every GeoJSON file in /sdcard/maps/overlays/ an overlay on view,
 * with the Layers and Refresh buttons and the info card on root, which
 * holds the view. bottom is the height of what is below the map (the
 * About line), which the card stays above. True when it put its buttons
 * at the top corners, there being a file to switch. */
bool maps_layers_open(map_view_t *view, lv_obj_t *root, int bottom);

/* Destroys the overlays, before the view. */
void maps_layers_close(void);
