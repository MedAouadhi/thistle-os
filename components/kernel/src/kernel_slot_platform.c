/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the flash side of the kernel slots, for ota.rs
 *
 * Every call into ESP-IDF's partition and OTA APIs is here, so that ota.rs
 * keeps to deciding (kernel_slot.rs) and the simulator needs none of it; it
 * keeps its slots as files instead.
 *
 * A slot is always named by its label, `kernel_a` or `kernel_b`
 * (partitions.csv). esp_ota_get_next_update_partition() is never asked:
 * from kernel_b it answers with the reserved Recovery slot, ota_2.
 */

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const esp_partition_t *kernel_slot(const char *label)
{
    if (!label) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

/* The running slot's label, and whether its image is new: pending
 * verification, to be kept or rolled back. */
esp_err_t kslot_platform_running(char *label, size_t len, bool *pending)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || !label || len == 0) return ESP_ERR_NOT_FOUND;
    strncpy(label, running->label, len - 1);
    label[len - 1] = '\0';
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    *pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
               state == ESP_OTA_IMG_PENDING_VERIFY;
    return ESP_OK;
}

/* The running kernel's own version, from its app description. */
const char *kslot_platform_running_version(void)
{
    return esp_app_get_description()->version;
}

/* Erase the slot and start writing an image of `size` bytes into it. */
esp_err_t kslot_platform_begin(const char *label, size_t size, uint32_t *handle)
{
    const esp_partition_t *slot = kernel_slot(label);
    if (!slot) return ESP_ERR_NOT_FOUND;
    if (slot == esp_ota_get_running_partition()) return ESP_ERR_INVALID_STATE;
    if (size > slot->size) return ESP_ERR_INVALID_SIZE;
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(slot, size, &h);
    *handle = (uint32_t)h;
    return err;
}

esp_err_t kslot_platform_write(uint32_t handle, const uint8_t *data, size_t len)
{
    return esp_ota_write((esp_ota_handle_t)handle, data, len);
}

void kslot_platform_abort(uint32_t handle)
{
    esp_ota_abort((esp_ota_handle_t)handle);
}

/* Finish the image, let ESP-IDF validate it, and select the slot to boot
 * next. A failure leaves the boot choice as it was. */
esp_err_t kslot_platform_finish(uint32_t handle, const char *label)
{
    esp_err_t err = esp_ota_end((esp_ota_handle_t)handle);
    if (err != ESP_OK) return err;
    const esp_partition_t *slot = kernel_slot(label);
    if (!slot) return ESP_ERR_NOT_FOUND;
    return esp_ota_set_boot_partition(slot);
}

/* Keep the running kernel: a reset boots it again rather than rolling back. */
esp_err_t kslot_platform_keep(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

/* Give the running kernel up and boot the other slot's. Restarts. */
esp_err_t kslot_platform_roll_back(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
