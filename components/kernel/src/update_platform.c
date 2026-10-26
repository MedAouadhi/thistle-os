/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what a system update keeps in NVS, for update_manager.rs
 *
 * The security floor is Recovery's own key, `thistle_sec`/`fw_sec_ver`, a
 * u32, so the two agree (ADR-0010). Beside it, each kernel slot's record:
 * the version and security version of the kernel written into it, as
 * `kernel_a_ver` / `kernel_a_sec` and `kernel_b_ver` / `kernel_b_sec`.
 * A slot's record is written when the slot is, and by a kernel for its own
 * slot when it runs. The simulator keeps the same in a file instead.
 */

#include "esp_err.h"
#include "nvs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NVS_NAMESPACE "thistle_sec"
#define FLOOR_KEY     "fw_sec_ver"

/* NVS keys hold 15 characters: "kernel_a_ver" is 12. */
static esp_err_t slot_keys(const char *slot, char *ver_key, char *sec_key)
{
    if (!slot || (strcmp(slot, "kernel_a") != 0 && strcmp(slot, "kernel_b") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(ver_key, 16, "%s_ver", slot);
    snprintf(sec_key, 16, "%s_sec", slot);
    return ESP_OK;
}

/* The floor; 0 while nothing has set it. */
esp_err_t update_platform_floor(uint32_t *floor)
{
    *floor = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_u32(nvs, FLOOR_KEY, floor);
    nvs_close(nvs);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t update_platform_set_floor(uint32_t floor)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(nvs, FLOOR_KEY, floor);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t update_platform_record(const char *slot, const char *version, uint32_t security_version)
{
    char ver_key[16], sec_key[16];
    esp_err_t err = slot_keys(slot, ver_key, sec_key);
    if (err != ESP_OK || !version) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, ver_key, version);
    if (err == ESP_OK) err = nvs_set_u32(nvs, sec_key, security_version);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* ESP_ERR_NOT_FOUND when the slot has no record. */
esp_err_t update_platform_get_record(const char *slot, char *version, size_t len,
                                     uint32_t *security_version)
{
    char ver_key[16], sec_key[16];
    esp_err_t err = slot_keys(slot, ver_key, sec_key);
    if (err != ESP_OK || !version || len == 0) return ESP_ERR_INVALID_ARG;
    version[0] = '\0';
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    size_t got = len;
    err = nvs_get_str(nvs, ver_key, version, &got);
    if (err == ESP_OK) err = nvs_get_u32(nvs, sec_key, security_version);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}
