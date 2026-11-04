/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Chat app lifecycle
 *
 * App ID  : com.thistle.tailchat
 * Name    : Chat
 *
 * Tailnet chat's screens: conversations, contacts and contact requests.
 * The kernel (tailnet_chat.rs) receives whether or not the app is open; the
 * app only reads and asks it (thistle/tailnet_chat.h).
 */
#include "tailchat/tailchat_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "tailchat";

static esp_err_t tailchat_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return tailchat_ui_create(ui_manager_get_app_area());
}

static void tailchat_on_start(void)
{
    statusbar_set_title("Chat");
}

static void tailchat_on_resume(void)
{
    statusbar_set_title("Chat");
}

static void tailchat_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    tailchat_ui_destroy();
}

static const app_manifest_t tailchat_manifest = {
    .id               = "com.thistle.tailchat",
    .name             = "Chat",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t tailchat_entry = {
    .manifest   = &tailchat_manifest,
    .on_create  = tailchat_on_create,
    .on_start   = tailchat_on_start,
    .on_resume  = tailchat_on_resume,
    .on_destroy = tailchat_on_destroy,
};

esp_err_t tailchat_app_register(void)
{
    return app_manager_register(&tailchat_entry);
}
