/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Podcasts app lifecycle
 *
 * The pages are made on create and deleted on destroy, as every app's are;
 * the player and the downloads are not the pages' and go on between the two
 * (pod_player.h). The quick panel gets a Podcast switch that pauses and
 * resumes the player from any app.
 */
#include "podcasts/podcasts_app.h"
#include "podcasts/pod_player.h"

#include "thistle/app_manager.h"
#include "ui/manager.h"
#include "ui/quick_panel.h"
#include "ui/statusbar.h"
#include "esp_log.h"

static const char *TAG = "podcasts";

/* ------------------------------------------------------------------ */
/* The quick panel's switch                                             */
/* ------------------------------------------------------------------ */

/* Shown while an episode is loaded and not stopped. */
static bool qp_available(void)
{
    return pod_player_active();
}

static bool qp_is_on(void)
{
    pod_player_status_t st;
    pod_player_status(&st);
    return st.state == POD_PLAYER_PLAYING || st.state == POD_PLAYER_LOADING;
}

static const char *qp_set_on(bool on)
{
    if (on) pod_player_resume();
    else pod_player_pause();
    return NULL;
}

static const quick_toggle_t s_quick_toggle = {
    .symbol    = LV_SYMBOL_AUDIO,
    .label     = "Podcast",
    .available = qp_available,
    .is_on     = qp_is_on,
    .set_on    = qp_set_on,
};

/* ------------------------------------------------------------------ */
/* Lifecycle callbacks                                                  */
/* ------------------------------------------------------------------ */

static int podcasts_on_create(void)
{
    ESP_LOGI(TAG, "on_create");
    return podcasts_ui_create(ui_manager_get_app_area());
}

static void podcasts_on_start(void)
{
    ESP_LOGI(TAG, "on_start");
    statusbar_set_title("Podcasts");
}

static void podcasts_on_resume(void)
{
    ESP_LOGI(TAG, "on_resume");
    statusbar_set_title("Podcasts");
}

static void podcasts_on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy");
    podcasts_ui_destroy();
}

/* ------------------------------------------------------------------ */
/* App manifest                                                         */
/* ------------------------------------------------------------------ */

static const app_manifest_t podcasts_manifest = {
    .id               = "com.thistle.podcasts",
    .name             = "Podcasts",
    .version          = "0.1.0",
    .allow_background = true,
};

static app_entry_t podcasts_entry = {
    .manifest   = &podcasts_manifest,
    .on_create  = podcasts_on_create,
    .on_start   = podcasts_on_start,
    .on_resume  = podcasts_on_resume,
    .on_destroy = podcasts_on_destroy,
};

esp_err_t podcasts_app_register(void)
{
    quick_panel_add_toggle(&s_quick_toggle);
    return app_manager_register(&podcasts_entry);
}
