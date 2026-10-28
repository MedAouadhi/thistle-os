#pragma once
#include "esp_err.h"
#include "lvgl.h"

esp_err_t settings_app_register(void);
esp_err_t settings_ui_create(lv_obj_t *parent);
void settings_ui_show(void);
void settings_ui_hide(void);
void settings_ui_destroy(void);

/* The next time Settings opens, it opens at `page`, an item of its main list
 * ("System update"), rather than at the list; Back returns to the list.
 * Call it just before app_manager_launch("com.thistle.settings"). */
void settings_app_open_next(const char *page);
