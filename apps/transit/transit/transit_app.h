/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Transit app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t transit_app_register(void);
esp_err_t transit_ui_create(lv_obj_t *parent);
void transit_ui_destroy(void);
