/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Phone app public interface
 *
 * Contacts from every contacts.vcf on the SD card, favourites, recent calls
 * and a keypad, with a Call and a Message button on every number. The kernel
 * keeps the contacts (thistle/phonebook.h) and runs the calls
 * (thistle/call.h); the call screen is the system's, not this app's.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t phone_app_register(void);

/* The app's pages, under `parent` (phone_ui.c). */
esp_err_t phone_ui_create(lv_obj_t *parent);
void phone_ui_destroy(void);
