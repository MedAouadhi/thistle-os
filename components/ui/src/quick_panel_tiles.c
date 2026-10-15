/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the quick panel's built-in switches and sliders
 *
 * Each is a few lines over the kernel call that owns its state, so the
 * panel, Settings and apps all agree on it, and each choice is saved there.
 * A feature with a switch of its own adds it the same way, from its own
 * code, with quick_panel_add_toggle().
 */

#include "ui/quick_panel.h"
#include "thistle/audio_volume.h"
#include "thistle/gps_manager.h"
#include "thistle/light_prefs.h"
#include "thistle/mesh.h"
#include "thistle/net_manager.h"
#include "thistle/tailnet.h"
#include "thistle/wifi_manager.h"

#include "lvgl.h"

#define NOT_SAVED "Not saved for next boot"

/* What to say after switching to `on`: nothing if it switched and was saved. */
static const char *outcome(bool now_on, bool on, esp_err_t err,
                           const char *did_not_start, const char *did_not_stop)
{
    if (now_on != on) {
        return on ? did_not_start : did_not_stop;
    }
    return err == ESP_OK ? NULL : NOT_SAVED;
}

/* --- WiFi --- */

static const char *wifi_set(bool on)
{
    esp_err_t err = wifi_manager_set_enabled(on);
    return outcome(wifi_manager_is_enabled(), on, err,
                   "WiFi did not start", "WiFi is not available");
}

static const quick_toggle_t s_wifi = {
    .symbol = LV_SYMBOL_WIFI,
    .label  = "WiFi",
    .is_on  = wifi_manager_is_enabled,
    .set_on = wifi_set,
};

/* --- Mobile data --- */

static bool mobile_is_on(void)
{
    net_cellular_status_t st;
    return net_cellular_get_status(&st) == ESP_OK && st.enabled;
}

static const char *mobile_set(bool on)
{
    esp_err_t err = net_cellular_set_enabled(on);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return "No modem on this board";
    }
    /* On is the choice; the modem finds a network in its own time, which
     * the status bar shows. */
    return outcome(mobile_is_on(), on, err,
                   "Mobile data did not start", "Mobile data did not stop");
}

static const quick_toggle_t s_mobile = {
    .symbol    = "LTE",
    .label     = "Mobile",
    .available = net_cellular_present,
    .is_on     = mobile_is_on,
    .set_on    = mobile_set,
};

/* --- Tailscale: the tailnet. A device that has never joined has to sign in
 * once, which only the app can show. --- */

static const char *tailnet_set(bool on)
{
    esp_err_t err = tailnet_set_enabled(on);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return "No Tailscale on this board";
    }
    const char *said = outcome(tailnet_is_enabled(), on, err,
                               "Tailscale did not start", "Tailscale did not stop");
    if (said || !on) {
        return said;
    }
    tailnet_status_t st;
    if (tailnet_get_status(&st) == ESP_OK && !st.joined) {
        return "Open Tailscale to sign in";
    }
    return NULL;
}

static const quick_toggle_t s_tailnet = {
    .symbol    = LV_SYMBOL_SHUFFLE,
    .label     = "Tailscale",
    .available = tailnet_available,
    .is_on     = tailnet_is_enabled,
    .set_on    = tailnet_set,
};

/* --- GPS --- */

static const char *gps_set(bool on)
{
    esp_err_t err = rs_gps_set_enabled(on);
    return outcome(rs_gps_is_enabled(), on, err, "GPS did not start", "GPS did not stop");
}

static const quick_toggle_t s_gps = {
    .symbol    = LV_SYMBOL_GPS,
    .label     = "GPS",
    .available = rs_gps_available,
    .is_on     = rs_gps_is_enabled,
    .set_on    = gps_set,
};

/* --- LoRa: the MeshCore node, and with it the radio --- */

static const char *lora_set(bool on)
{
    esp_err_t err = rs_mesh_set_enabled(on);
    return outcome(rs_mesh_is_enabled(), on, err, "LoRa did not start", "LoRa did not stop");
}

static const quick_toggle_t s_lora = {
    .symbol    = LV_SYMBOL_LOOP,
    .label     = "LoRa",
    .available = rs_mesh_available,
    .is_on     = rs_mesh_is_enabled,
    .set_on    = lora_set,
};

/* --- Key light: back to the level it was last lit at --- */

static bool keys_lit(void)
{
    return light_keys_level() > 0;
}

static const char *keys_set(bool on)
{
    esp_err_t err = light_set_keys(on ? light_keys_on_level() : 0, true);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return "No key light on this board";
    }
    return outcome(keys_lit(), on, err, "Key light did not come on", "Key light did not go off");
}

static const quick_toggle_t s_keys = {
    .symbol    = LV_SYMBOL_KEYBOARD,
    .label     = "Key light",
    .available = light_keys_available,
    .is_on     = keys_lit,
    .set_on    = keys_set,
};

/* --- Mute --- */

static const char *mute_set(bool on)
{
    esp_err_t err = rs_audio_set_muted(on);
    return outcome(rs_audio_is_muted(), on, err, "Did not mute", "Did not unmute");
}

static const quick_toggle_t s_mute = {
    .symbol    = LV_SYMBOL_MUTE,
    .label     = "Mute",
    .available = rs_audio_available,
    .is_on     = rs_audio_is_muted,
    .set_on    = mute_set,
};

/* --- Screen light --- */

static const char *screen_light_set(uint8_t percent, bool save)
{
    esp_err_t err = light_set_display(percent, save);
    if (err == ESP_OK) {
        return NULL;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return "No screen light on this board";
    }
    return light_display_level() == percent ? NOT_SAVED : "Screen light did not change";
}

static const quick_level_t s_screen_light = {
    .symbol    = LV_SYMBOL_EYE_OPEN,
    .label     = "Screen light",
    .available = light_display_available,
    .min       = light_display_min,
    .get       = light_display_level,
    .set       = screen_light_set,
};

/* --- Volume: turning it up unmutes, as a phone's does --- */

static const char *volume_set(uint8_t percent, bool save)
{
    if (percent > 0 && rs_audio_is_muted() && rs_audio_set_muted(false) != ESP_OK) {
        return NOT_SAVED;
    }
    esp_err_t err = rs_audio_set_volume(percent, save);
    if (err == ESP_OK) {
        return NULL;
    }
    return rs_audio_get_volume() == percent ? NOT_SAVED : "Volume did not change";
}

static const quick_level_t s_volume = {
    .symbol    = LV_SYMBOL_VOLUME_MAX,
    .label     = "Volume",
    .available = rs_audio_available,
    .get       = rs_audio_get_volume,
    .set       = volume_set,
};

void quick_panel_add_builtin(void)
{
    quick_panel_add_toggle(&s_wifi);
    quick_panel_add_toggle(&s_mobile);
    quick_panel_add_toggle(&s_tailnet);
    quick_panel_add_toggle(&s_gps);
    quick_panel_add_toggle(&s_lora);
    quick_panel_add_toggle(&s_keys);
    quick_panel_add_toggle(&s_mute);
    quick_panel_add_level(&s_screen_light);
    quick_panel_add_level(&s_volume);
}
