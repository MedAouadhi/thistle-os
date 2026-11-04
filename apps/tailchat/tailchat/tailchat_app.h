/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Chat (Tailnet chat) app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t tailchat_app_register(void);
esp_err_t tailchat_ui_create(lv_obj_t *parent);
void tailchat_ui_destroy(void);
