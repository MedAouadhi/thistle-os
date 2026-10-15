/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS Simulator — mDNS (espressif/mdns)
 *
 * Nothing answers: the host has its own name. Each call says what a device
 * would do, so a test can see it was asked.
 */
#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

static inline esp_err_t mdns_init(void)
{
    return ESP_OK;
}

static inline void mdns_free(void)
{
    printf("[sim_mdns] stopped\n");
}

static inline esp_err_t mdns_hostname_set(const char *hostname)
{
    printf("[sim_mdns] would answer as %s.local\n", hostname);
    return ESP_OK;
}

static inline esp_err_t mdns_instance_name_set(const char *instance_name)
{
    (void)instance_name;
    return ESP_OK;
}

static inline esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                                         const char *proto, uint16_t port, mdns_txt_item_t txt[],
                                         size_t num_items)
{
    (void)instance_name;
    (void)txt;
    (void)num_items;
    printf("[sim_mdns] would offer %s.%s on port %u\n", service_type, proto, port);
    return ESP_OK;
}
