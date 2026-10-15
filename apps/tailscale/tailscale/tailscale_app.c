/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Tailscale app lifecycle
 *
 * App ID  : com.thistle.tailscale
 * Name    : Tailscale
 *
 * The app only shows and switches: the kernel keeps the device on the
 * tailnet (thistle/tailnet.h), with the app open or not.
 */
#include "tailscale/tailscale_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "tailscale";

static esp_err_t tailscale_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return tailscale_ui_create(ui_manager_get_app_area());
}

static void tailscale_on_start(void)
{
    statusbar_set_title("Tailscale");
}

static void tailscale_on_resume(void)
{
    statusbar_set_title("Tailscale");
}

static void tailscale_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    tailscale_ui_destroy();
}

static const app_manifest_t tailscale_manifest = {
    .id               = "com.thistle.tailscale",
    .name             = "Tailscale",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t tailscale_entry = {
    .manifest   = &tailscale_manifest,
    .on_create  = tailscale_on_create,
    .on_start   = tailscale_on_start,
    .on_resume  = tailscale_on_resume,
    .on_destroy = tailscale_on_destroy,
};

esp_err_t tailscale_app_register(void)
{
    return app_manager_register(&tailscale_entry);
}
