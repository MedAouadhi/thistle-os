/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Maps app's overlays: the Pack's own (a transit network)
 * and the GeoJSON files on the card
 */
#pragma once

#include "map_engine/map_view.h"
#include "lvgl.h"

/* Makes each overlay baked into pack, and every GeoJSON file in
 * /sdcard/maps/overlays/, an overlay on view, with the Layers button (and
 * Refresh, for files) and the info card on root, which holds the view.
 * bottom is the height of what is below the map (the About line), which
 * the card stays above. True when it put Refresh at the top right, there
 * being a file to read again. */
bool maps_layers_open(map_view_t *view, map_pack_t *pack, lv_obj_t *root, int bottom);

/* Destroys the overlays, before the view. */
void maps_layers_close(void);
