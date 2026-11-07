/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — SMS app lifecycle
 *
 * App ID  : com.thistle.sms
 * Name    : SMS
 *
 * Text messages through the modem: a conversation with each number, or with
 * the name a sender such as a bank gives. The kernel receives them
 * (sms_manager.rs) whether or not the app is open; the Phone app's Message
 * opens a conversation here (rs_sms_request_compose).
 */
#include "sms/sms_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "sms";

static esp_err_t sms_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return sms_ui_create(ui_manager_get_app_area());
}

static void sms_on_start(void)
{
    statusbar_set_title("SMS");
}

static void sms_on_resume(void)
{
    statusbar_set_title("SMS");
}

static void sms_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    sms_ui_destroy();
}

static const app_manifest_t sms_manifest = {
    .id               = "com.thistle.sms",
    .name             = "SMS",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t sms_entry = {
    .manifest   = &sms_manifest,
    .on_create  = sms_on_create,
    .on_start   = sms_on_start,
    .on_resume  = sms_on_resume,
    .on_destroy = sms_on_destroy,
};

esp_err_t sms_app_register(void)
{
    return app_manager_register(&sms_entry);
}
