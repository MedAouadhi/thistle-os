/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Transit app lifecycle
 *
 * App ID  : com.thistle.transit
 * Name    : Transit
 *
 * A map of the card's Map Pack with a city's stops and moving vehicles
 * over it, from its Provider (transit_provider.h), fetched only while the
 * app is open (transit_session.h); a tapped stop shows its departures
 * (transit_ui.c). Leaving the app destroys it, which stops the fetching.
 */
#include "transit/transit_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "transit";

static esp_err_t transit_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return transit_ui_create(ui_manager_get_app_area());
}

static void transit_on_start(void)
{
    statusbar_set_title("Transit");
}

static void transit_on_resume(void)
{
    statusbar_set_title("Transit");
}

static void transit_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    transit_ui_destroy();
}

static const app_manifest_t transit_manifest = {
    .id               = "com.thistle.transit",
    .name             = "Transit",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t transit_entry = {
    .manifest   = &transit_manifest,
    .on_create  = transit_on_create,
    .on_start   = transit_on_start,
    .on_resume  = transit_on_resume,
    .on_destroy = transit_on_destroy,
};

esp_err_t transit_app_register(void)
{
    return app_manager_register(&transit_entry);
}
