/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer app lifecycle
 *
 * App ID  : com.thistle.filexfer
 * Name    : File Transfer
 *
 * The server runs while the app is open: it starts, with a new password,
 * when the app is made, and stops when the app is left, which destroys it.
 */
#include "filexfer/filexfer_app.h"
#include "filexfer/xfer_server.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "filexfer";

static esp_err_t filexfer_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    xfer_server_start();
    return filexfer_ui_create(ui_manager_get_app_area());
}

static void filexfer_on_start(void)
{
    statusbar_set_title("File Transfer");
}

static void filexfer_on_resume(void)
{
    statusbar_set_title("File Transfer");
}

static void filexfer_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    filexfer_ui_destroy();
    xfer_server_stop();
}

static const app_manifest_t filexfer_manifest = {
    .id               = "com.thistle.filexfer",
    .name             = "File Transfer",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t filexfer_entry = {
    .manifest   = &filexfer_manifest,
    .on_create  = filexfer_on_create,
    .on_start   = filexfer_on_start,
    .on_resume  = filexfer_on_resume,
    .on_destroy = filexfer_on_destroy,
};

esp_err_t filexfer_app_register(void)
{
    return app_manager_register(&filexfer_entry);
}
