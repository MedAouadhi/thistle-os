/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — To-do app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t todo_app_register(void);
esp_err_t todo_ui_create(lv_obj_t *parent);
void todo_ui_destroy(void);
