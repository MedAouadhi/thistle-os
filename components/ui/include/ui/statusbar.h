#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Create the status bar on the given parent */
esp_err_t statusbar_create(lv_obj_t *parent);

/* Update status bar content */
void statusbar_set_battery(uint8_t percent, bool charging);
void statusbar_set_wifi(bool connected, int rssi);
void statusbar_set_title(const char *title);
void statusbar_set_time(uint8_t hour, uint8_t minute);
void statusbar_set_time_str(const char *time_str);

/* Show or leave out the bar's clock. Home leaves it out while it shows the
 * time large; every other screen has it. */
void statusbar_set_clock_visible(bool visible);

/* Get status bar height in pixels */
uint16_t statusbar_get_height(void);

/* Refresh status bar colors after a theme change */
void statusbar_refresh_theme(void);
