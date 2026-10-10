#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Initialize e-paper refresh tracker */
esp_err_t epaper_refresh_init(uint16_t display_width, uint16_t display_height);

/* Mark an area as dirty */
void epaper_refresh_mark_dirty(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* Mark entire screen as dirty */
void epaper_refresh_mark_full(void);

/* Check if any area is dirty */
bool epaper_refresh_is_dirty(void);

/* Get the bounding box of all dirty areas */
void epaper_refresh_get_bounds(uint16_t *x1, uint16_t *y1, uint16_t *x2, uint16_t *y2);

/* Clear dirty state after refresh */
void epaper_refresh_clear(void);

/* Get refresh counter (increments with each refresh cycle) */
uint32_t epaper_refresh_get_count(void);

/* How the panel is refreshed once its first paint is done, which the driver
 * always makes full. The kernel's refresh_prefs keeps the choice in
 * system.json and mirrors these values. */
typedef enum {
    EPAPER_REFRESH_POLICY_FAST = 0,  /* fast every time; ghosting builds up    */
    EPAPER_REFRESH_POLICY_FULL = 1,  /* full every time; clean, but it flashes */
    EPAPER_REFRESH_POLICY_AUTO = 2,  /* fast, then a full one after every run  */
} epaper_refresh_policy_t;

/* Fast refreshes Auto allows before a full one. The GDEQ031T10 datasheet
 * recommends "a full-screen refresh after 5 consecutive operations to reduce
 * the accumulation of afterimages". */
#define EPAPER_REFRESH_AUTO_FAST_RUN 5

/* Set the policy. A value outside the enum is taken as FAST. */
void epaper_refresh_set_policy(epaper_refresh_policy_t policy);
epaper_refresh_policy_t epaper_refresh_get_policy(void);

/* Make the next refresh full whatever the policy, to clear what a splash
 * screen or a whole-screen change leaves behind. Safe from any task. */
void epaper_refresh_request_full(void);

/* Decide the refresh about to happen: true for full, false for fast. Call it
 * once per panel refresh, just before it — it counts the refresh. */
bool epaper_refresh_next_is_full(void);
