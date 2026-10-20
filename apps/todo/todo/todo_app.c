/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — To-do app lifecycle
 *
 * App ID  : com.thistle.todo
 * Name    : To-do
 *
 * Every change is saved as it is made, so leaving the app has nothing left
 * to save.
 */
#include "todo/todo_app.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "todo";

static esp_err_t todo_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return todo_ui_create(ui_manager_get_app_area());
}

static void todo_on_start(void)
{
    statusbar_set_title("To-do");
}

static void todo_on_resume(void)
{
    statusbar_set_title("To-do");
}

static void todo_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    todo_ui_destroy();
}

static const app_manifest_t todo_manifest = {
    .id               = "com.thistle.todo",
    .name             = "To-do",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t todo_entry = {
    .manifest   = &todo_manifest,
    .on_create  = todo_on_create,
    .on_start   = todo_on_start,
    .on_resume  = todo_on_resume,
    .on_destroy = todo_on_destroy,
};

esp_err_t todo_app_register(void)
{
    return app_manager_register(&todo_entry);
}
