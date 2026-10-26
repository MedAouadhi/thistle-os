/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — system updates
 *
 * The kernel installs a firmware into the other kernel slot only once its
 * artifact manifest holds up: signed by the kernel's key, a firmware, for
 * this chip and this board, at or above the device's security floor, and
 * the bytes written of the manifest's size and SHA-256 (ADR-0010). The
 * decisions are update_policy.rs's, the running of them update_manager.rs's,
 * every NVS call update_platform.c's.
 *
 * A release comes from the catalog or the card. Once a day, with a network
 * up, the kernel reads the catalog (the App Store's, kept at
 * /sdcard/appstore/catalog.json) and offers the newest firmware newer than
 * the running one for this chip and board; a download streams it into the
 * other kernel slot, and on mobile data waits until the user allows it. A
 * release copied to the card installs too: its .bin, the manifest beside it
 * (<bin>.manifest) and the manifest's signature (<bin>.manifest.sig), as the
 * release assets are named, in /sdcard/update/. Settings → System update
 * shows both.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPDATE_IDLE = 0,
    UPDATE_CHECKING,
    UPDATE_UP_TO_DATE,
    UPDATE_AVAILABLE,
    UPDATE_DOWNLOADING,
    UPDATE_WRITING,      /* checking the release and writing it */
    UPDATE_READY,        /* written: runs after the next restart */
    UPDATE_FAILED,       /* refused or failed; reason says why */
    UPDATE_WAITING,      /* a download waits for a network, or on mobile
                          * data for the user's leave; reason says which */
} update_state_t;

typedef struct {
    uint32_t state;      /* update_state_t */
    uint32_t done;       /* bytes written so far */
    uint32_t total;      /* bytes to write; 0 until known */
    char version[40];    /* the version being or just installed */
    char reason[128];    /* why the last install failed */
} update_status_t;

void rs_update_get_status(update_status_t *out);

/* The running firmware's version. */
const char *rs_update_running_version(void);

/* Whether the card holds a release in update/; the version its manifest
 * names into `version` ("" if the manifest does not read). */
bool rs_update_card_release(char *version, size_t len);

/* Install the card's release on a task of its own; follow it with
 * rs_update_get_status(). ESP_ERR_INVALID_STATE while one is under way. */
esp_err_t rs_update_install_from_card(void);

/* The release the catalog offers, if any: its version and notes. */
bool rs_update_offer(char *version, size_t version_len, char *notes, size_t notes_len);

/* App updates: the installed apps (the App Store's packages) that the
 * catalog last read, by a check or the App Store's Refresh, lists a newer
 * release of: a higher release sequence, with a min_os the running firmware
 * meets. Recounted as apps are installed and removed. */
uint32_t rs_update_app_count(void);

/* Whether the installed app `id` is offered an update; the release's
 * version into `version`. */
bool rs_update_app_offer(const char *id, char *version, size_t len);

/* What the last check found ("up to date", "0.7.0 is available", "the
 * catalog could not be read") and when (0: the clock was unset). False if
 * no check has been made. */
bool rs_update_last_check(int64_t *when_s, char *found, size_t len);

/* Read the catalog now (UPDATE_CHECKING). ESP_ERR_INVALID_STATE while a
 * check or an install is under way. */
esp_err_t rs_update_check_now(void);

/* Download and install the release on offer. On mobile data it waits
 * (UPDATE_WAITING) unless mobile_data_allowed; with no network it waits for
 * one. ESP_ERR_NOT_FOUND with nothing on offer. */
esp_err_t rs_update_install(bool mobile_data_allowed);

/* Whether a download waits for the user to allow mobile data. */
bool rs_update_needs_consent(void);

/* Restart into the kernel just installed (UPDATE_READY); else nothing. */
void rs_update_restart(void);

/* Whether Settings offers to go back to the version in the other kernel
 * slot, with no download: only while that slot's NVS record is the version
 * its image describes and its security version is at or above the floor,
 * and no install is under way or waiting for its restart. The version into
 * `version`. */
bool rs_update_go_back_version(char *version, size_t len);

/* Go back: select the other kernel slot as it is and restart into it; its
 * kernel brings back its own board profile. Returns only when going back is
 * not offered (ESP_ERR_NOT_FOUND) or the slot cannot be selected. */
esp_err_t rs_update_go_back(void);

#ifdef __cplusplus
}
#endif
