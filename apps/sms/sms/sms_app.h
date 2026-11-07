/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — SMS app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t sms_app_register(void);
esp_err_t sms_ui_create(lv_obj_t *parent);
void sms_ui_destroy(void);
