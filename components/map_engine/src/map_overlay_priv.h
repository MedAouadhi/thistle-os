/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the map view and its overlays say to each other
 *
 * map_overlay.c keeps the items and draws them; map_view.c decides when,
 * and owns the list. Both on the UI task.
 */
#pragma once

#include "map_engine/map_overlay.h"

#include <stdint.h>

/* Where the overlays are drawn: the view's pixels and the world they show. */
typedef struct {
    int      zoom;
    int64_t  left, top;
    int      w, h;
} map_frame_t;

typedef struct {
    int on;          /* overlays switched on */
    int in_view;     /* their items that reach into the view */
    int decluttered; /* points and texts left out */
} map_overlay_stats_t;

typedef struct {
    map_overlay_t *overlay;  /* NULL: nothing within reach */
    int            item;
    uint32_t       id;
    int            distance; /* px, rounded */
} map_overlay_hit_t;

/* The view's side, map_view.c. */
void map_view_attach_overlay(map_view_t *view, map_overlay_t *overlay);
void map_view_detach_overlay(map_view_t *view, map_overlay_t *overlay);
void map_view_overlay_changed(map_view_t *view, map_overlay_t *overlay);
void map_view_overlay_switched(map_view_t *view, map_overlay_t *overlay);

/* The overlays' side, map_overlay.c. first is the view's list, in z order,
 * lowest first, linked by map_overlay_next(). */
map_overlay_t *map_overlay_next(const map_overlay_t *overlay);
void map_overlay_set_next(map_overlay_t *overlay, map_overlay_t *next);
int map_overlay_z(const map_overlay_t *overlay);
/* Calls overlay's tap callback, if it has one. */
void map_overlay_tap(map_overlay_t *overlay, int item, uint32_t id);

/* Draws every overlay switched on over pixels (w * h RGB565, the tiles
 * already there), decluttering, and notes what each item drew for
 * map_overlays_hit(). */
void map_overlays_draw(map_overlay_t *first, const map_frame_t *frame, uint16_t *pixels,
                       map_overlay_stats_t *stats);

/* The item drawn last nearest view pixel (x, y), within reach. */
map_overlay_hit_t map_overlays_hit(map_overlay_t *first, const map_frame_t *frame, int x, int y);
