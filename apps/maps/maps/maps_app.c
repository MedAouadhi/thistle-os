/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Maps app lifecycle
 *
 * App ID  : com.thistle.maps
 * Name    : Maps
 *
 * Shows the Map Pack on the card (maps_ui.c). The map itself is
 * components/map_engine's, which other built-in apps can embed too.
 */
#include "maps/maps_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "maps";

static esp_err_t maps_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return maps_ui_create(ui_manager_get_app_area());
}

static void maps_on_start(void)
{
    statusbar_set_title("Maps");
}

static void maps_on_resume(void)
{
    statusbar_set_title("Maps");
}

static void maps_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    maps_ui_destroy();
}

static const app_manifest_t maps_manifest = {
    .id               = "com.thistle.maps",
    .name             = "Maps",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t maps_entry = {
    .manifest   = &maps_manifest,
    .on_create  = maps_on_create,
    .on_start   = maps_on_start,
    .on_resume  = maps_on_resume,
    .on_destroy = maps_on_destroy,
};

esp_err_t maps_app_register(void)
{
    return app_manager_register(&maps_entry);
}
