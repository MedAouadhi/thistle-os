/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Maps app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t maps_app_register(void);
esp_err_t maps_ui_create(lv_obj_t *parent);
void maps_ui_destroy(void);
