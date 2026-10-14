/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Phone app lifecycle
 *
 * The pages are made on create and deleted on destroy, as every app's are.
 * Contacts and calls are the kernel's, so nothing is lost in between: the
 * app remembers only which tab was open and the number being typed.
 */
#include "phone/phone_app.h"
#include "reader/arabic_shape.h"

#include "hal/board.h"
#include "thistle/app_manager.h"
#include "ui/call_ui.h"
#include "ui/manager.h"
#include "ui/statusbar.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "phone";

LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);

/* The call screen's shaping of names: Arabic letters joined, as the app's
 * lists show them. */
static char *shape_name(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    return len && arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, NULL) : NULL;
}

/* The call screen names callers from the phonebook, whose names are not all
 * ASCII: it gets the Reader's font, as the app's lists have, its 1-bit cut
 * on e-paper. */
static void give_call_screen_font(void)
{
    const hal_registry_t *reg = hal_get_registry();
    bool mono = reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER;
    call_ui_set_text_font(mono ? &reader_font_16_mono : &reader_font_16, shape_name);
}

static esp_err_t phone_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return phone_ui_create(ui_manager_get_app_area());
}

static void phone_on_start(void)
{
    ESP_LOGI(TAG, "on_start");
    statusbar_set_title("Phone");
}

static void phone_on_resume(void)
{
    ESP_LOGI(TAG, "on_resume");
    statusbar_set_title("Phone");
}

static void phone_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    phone_ui_destroy();
}

static const app_manifest_t phone_manifest = {
    .id               = "com.thistle.phone",
    .name             = "Phone",
    .version          = "0.1.0",
    .allow_background = false,
};

static app_entry_t phone_entry = {
    .manifest   = &phone_manifest,
    .on_create  = phone_on_create,
    .on_start   = phone_on_start,
    .on_resume  = phone_on_resume,
    .on_destroy = phone_on_destroy,
};

esp_err_t phone_app_register(void)
{
    /* Registration runs once at startup under an LVGL window manager, with
     * the display registered and before any call can open the screen, which
     * only keeps a copy of the font. */
    give_call_screen_font();
    return app_manager_register(&phone_entry);
}
