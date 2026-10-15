/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t filexfer_app_register(void);

esp_err_t filexfer_ui_create(lv_obj_t *parent);
void filexfer_ui_destroy(void);
