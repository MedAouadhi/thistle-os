/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes app lifecycle
 *
 * App ID  : com.thistle.hermes
 * Name    : Hermes
 *
 * A Hermes Device Bridge client over the tailnet: the user's Hermes agent
 * as hermes_esp32's device has it. The Bridge Session lives while the app
 * is open; leaving it closes the session, and opening it again syncs from
 * the bridge. See .scratch/hermes/spec.md.
 */
#include "hermes/hermes_app.h"
#include "hermes/hermes_ui.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "hermes";

static esp_err_t hermes_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return hermes_ui_create(ui_manager_get_app_area());
}

static void hermes_on_start(void)
{
    statusbar_set_title("Hermes");
}

static void hermes_on_resume(void)
{
    statusbar_set_title("Hermes");
}

static void hermes_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    hermes_ui_destroy();
}

static const app_manifest_t hermes_manifest = {
    .id               = "com.thistle.hermes",
    .name             = "Hermes",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t hermes_entry = {
    .manifest   = &hermes_manifest,
    .on_create  = hermes_on_create,
    .on_start   = hermes_on_start,
    .on_resume  = hermes_on_resume,
    .on_destroy = hermes_on_destroy,
};

esp_err_t hermes_app_register(void)
{
    return app_manager_register(&hermes_entry);
}
