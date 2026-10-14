// SPDX-License-Identifier: BSD-3-Clause
// The one ESP-IDF header hal/network.h pulls in, for building the modem's
// parsers on the host. Values as ESP-IDF defines them.
#pragma once

typedef int esp_err_t;
#define ESP_OK                   0
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
