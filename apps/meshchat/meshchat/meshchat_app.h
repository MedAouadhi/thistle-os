/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — MeshCore app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t meshchat_app_register(void);
esp_err_t meshchat_ui_create(lv_obj_t *parent);
void meshchat_ui_destroy(void);
