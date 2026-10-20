// SPDX-License-Identifier: BSD-3-Clause
// Weather — the weather station, as a toolkit-neutral standalone app.
//
// No board has a barometer or a humidity sensor in the HAL yet, so those
// rows say so; the GPS gives the altitude and position, when something has
// it on and it has a fix. Refresh reads them again. Everything handed to the
// kernel is static: a device refuses pointers into internal RAM, where the
// UI task's stack is.

#include "thistle_app.h"

#include <stdio.h>
#include <string.h>

#define TAG "weather"

static thistle_widget_t s_screen = THISTLE_WIDGET_NONE;
static thistle_widget_t s_altitude = THISTLE_WIDGET_NONE;
static thistle_widget_t s_position = THISTLE_WIDGET_NONE;
static thistle_widget_t s_satellites = THISTLE_WIDGET_NONE;
static thistle_widget_t s_battery = THISTLE_WIDGET_NONE;
static thistle_widget_t s_updated = THISTLE_WIDGET_NONE;
static thistle_gps_position_t s_position_now;

/* A coordinate as degrees to five places. The kernel's printf does the
 * arithmetic: the app does none on a double, which on a chip without a
 * double FPU means libgcc, and a RISC-V libgcc cannot go into a shared
 * object. The sign comes from the bits. */
static void format_degrees(char *out, size_t size, double degrees, char positive, char negative)
{
    uint64_t bits;
    memcpy(&bits, &degrees, sizeof bits);
    char hemisphere = (bits >> 63) ? negative : positive;
    bits &= ~((uint64_t)1 << 63);
    memcpy(&degrees, &bits, sizeof bits);
    snprintf(out, size, "%.5f %c", degrees, hemisphere);
}

/* Whole metres, toward zero, from a float's bits, for the same reason. */
static long whole(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    int exponent = (int)((bits >> 23) & 0xff) - 127;
    if (exponent < 0 || exponent > 30) return 0;
    uint32_t mantissa = (bits & 0x7fffff) | 0x800000;
    long magnitude = (long)(exponent >= 23 ? mantissa << (exponent - 23) : mantissa >> (23 - exponent));
    return (bits >> 31) ? -magnitude : magnitude;
}

static void read_and_show(void)
{
    char text[64];

    if (thistle_gps_get_position(&s_position_now) != 0) {
        thistle_ui_set_text(s_altitude, "Altitude:  GPS off");
        thistle_ui_set_text(s_position, "Position:  switch the GPS on");
        thistle_ui_set_text(s_satellites, "Satellites:  --");
    } else {
        snprintf(text, sizeof text, "Satellites:  %u", (unsigned)s_position_now.satellites);
        thistle_ui_set_text(s_satellites, text);
        if (s_position_now.fix_valid) {
            snprintf(text, sizeof text, "Altitude:  %ld m (GPS)", whole(s_position_now.altitude_m));
            thistle_ui_set_text(s_altitude, text);
            char lat[20];
            char lon[20];
            format_degrees(lat, sizeof lat, s_position_now.latitude, 'N', 'S');
            format_degrees(lon, sizeof lon, s_position_now.longitude, 'E', 'W');
            snprintf(text, sizeof text, "Position:  %s, %s", lat, lon);
            thistle_ui_set_text(s_position, text);
        } else {
            thistle_ui_set_text(s_altitude, "Altitude:  no GPS fix yet");
            thistle_ui_set_text(s_position, "Position:  no GPS fix yet");
        }
    }

    snprintf(text, sizeof text, "Battery:  %u%%", (unsigned)thistle_power_get_battery_pct());
    thistle_ui_set_text(s_battery, text);

    unsigned long seconds = thistle_millis() / 1000;
    snprintf(text, sizeof text, "Updated at %lu:%02lu:%02lu since start", seconds / 3600,
             (seconds / 60) % 60, seconds % 60);
    thistle_ui_set_text(s_updated, text);
}

static void refresh_clicked(thistle_widget_t widget, int event, void *user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;
    thistle_log(TAG, "refresh");
    read_and_show();
}

static thistle_widget_t row(const char *text, uint32_t color)
{
    thistle_widget_t label = thistle_ui_create_label(s_screen, text);
    thistle_ui_set_font_size(label, 14);
    thistle_ui_set_text_color(label, color);
    return label;
}

static int weather_on_create(void)
{
    thistle_log(TAG, "on_create");
    uint32_t text = thistle_ui_theme_text();
    uint32_t secondary = thistle_ui_theme_text_secondary();

    s_screen = thistle_ui_create_container(thistle_ui_get_app_root());
    thistle_ui_set_size(s_screen, -1, -1);
    thistle_ui_set_layout(s_screen, THISTLE_LAYOUT_FLEX_COLUMN);
    thistle_ui_set_gap(s_screen, 8);
    thistle_ui_set_padding(s_screen, 10, 10, 10, 10);
    thistle_ui_set_bg_color(s_screen, thistle_ui_theme_bg());
    thistle_ui_set_scrollable(s_screen, true);

    thistle_widget_t heading = thistle_ui_create_label(s_screen, "Weather Station");
    thistle_ui_set_font_size(heading, 18);
    thistle_ui_set_text_color(heading, text);

    row("Temperature:  no sensor", secondary);
    row("Pressure:  no sensor", secondary);
    row("Humidity:  no sensor", secondary);
    s_altitude = row("Altitude:  --", text);
    s_position = row("Position:  --", text);
    s_satellites = row("Satellites:  --", text);
    s_battery = row("Battery:  --", text);
    s_updated = row("", secondary);

    thistle_widget_t refresh = thistle_ui_create_button(s_screen, "Refresh");
    thistle_ui_set_size(refresh, 110, 34);
    thistle_ui_set_bg_color(refresh, thistle_ui_theme_primary());
    thistle_ui_set_text_color(refresh, 0xFFFFFF);
    thistle_ui_set_radius(refresh, 6);
    thistle_ui_on_event(refresh, THISTLE_EVENT_CLICK, refresh_clicked, 0);

    read_and_show();
    return 0;
}

static void weather_on_start(void) { thistle_log(TAG, "on_start"); }
static void weather_on_pause(void) { thistle_log(TAG, "on_pause"); }
static void weather_on_resume(void) { read_and_show(); }

/* Leaving an app destroys it; the next open builds its screen again. */
static void weather_on_destroy(void)
{
    thistle_log(TAG, "on_destroy");
    thistle_ui_destroy(s_screen);
    s_screen = s_altitude = s_position = s_satellites = s_battery = s_updated =
        THISTLE_WIDGET_NONE;
}

static const thistle_app_t weather_app = {
    .id = "com.thistle.weather",
    .name = "Weather",
    .version = "1.0.0",
    .allow_background = false,
    .on_create = weather_on_create,
    .on_start = weather_on_start,
    .on_pause = weather_on_pause,
    .on_resume = weather_on_resume,
    .on_destroy = weather_on_destroy,
};

THISTLE_APP(weather_app);
