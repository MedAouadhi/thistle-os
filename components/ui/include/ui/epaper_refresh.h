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

/* How long the screen must go undrawn before the panel refreshes. LVGL draws
 * every 33 ms, and a change it draws over two of those, such as an app that
 * builds its page in an lv_async_call, still lands in one refresh. */
#define EPAPER_REFRESH_SETTLE_US   (60 * 1000)

/* The longest a change waits for the screen to settle, timed from its first
 * flush: something animating for good is shown anyway. */
#define EPAPER_REFRESH_MAX_WAIT_US (1000 * 1000)

/* While a finger is on the screen or Enter is held, how long the screen must
 * go undrawn instead. What LVGL draws then is a button pressed, black on
 * e-paper, about to be let go; shown at once, every tap, and every drag that
 * began on an app icon, flashed it black for a whole refresh. This is LVGL's
 * long press, which is shown. Nor does the cap above apply: a refresh started
 * mid-drag only kept the frame the list stopped on waiting for it. */
#define EPAPER_REFRESH_HELD_US     (400 * 1000)

/* How long after a finger or Enter lets go it still counts as held: LVGL
 * reads the release up to one 33 ms pass later and draws it a pass after. */
#define EPAPER_REFRESH_LET_GO_US   (100 * 1000)

/* Whether what LVGL has drawn should go to the panel now, given the time,
 * when the first and the last flush since the last refresh were, and when a
 * finger or Enter was last held (now, if one is). esp_timer microseconds. */
bool epaper_refresh_settled(int64_t now_us, int64_t first_flush_us, int64_t last_flush_us,
                            int64_t last_held_us);
