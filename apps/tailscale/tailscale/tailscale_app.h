// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t tailscale_app_register(void);

/* tailscale_ui.c */
esp_err_t tailscale_ui_create(lv_obj_t *parent);
void tailscale_ui_destroy(void);
