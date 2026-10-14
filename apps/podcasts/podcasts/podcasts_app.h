/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Podcasts app public interface
 */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t podcasts_app_register(void);
esp_err_t podcasts_ui_create(lv_obj_t *parent);
void podcasts_ui_destroy(void);
