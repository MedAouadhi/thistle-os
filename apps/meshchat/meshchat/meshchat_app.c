/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — MeshCore app lifecycle
 *
 * App ID  : com.thistle.meshchat
 * Name    : MeshCore
 *
 * The mesh's messages over LoRa: the Public channel every MeshCore node
 * shares, and a conversation with each node that writes or is written to.
 * The kernel runs the node (mesh_manager.rs) whether or not the app is open.
 */
#include "meshchat/meshchat_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "meshchat";

static esp_err_t meshchat_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return meshchat_ui_create(ui_manager_get_app_area());
}

static void meshchat_on_start(void)
{
    statusbar_set_title("MeshCore");
}

static void meshchat_on_resume(void)
{
    statusbar_set_title("MeshCore");
}

static void meshchat_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    meshchat_ui_destroy();
}

static const app_manifest_t meshchat_manifest = {
    .id               = "com.thistle.meshchat",
    .name             = "MeshCore",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t meshchat_entry = {
    .manifest   = &meshchat_manifest,
    .on_create  = meshchat_on_create,
    .on_start   = meshchat_on_start,
    .on_resume  = meshchat_on_resume,
    .on_destroy = meshchat_on_destroy,
};

esp_err_t meshchat_app_register(void)
{
    return app_manager_register(&meshchat_entry);
}
