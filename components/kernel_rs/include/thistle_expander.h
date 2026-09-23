/*
 * The one runtime call a driver has onto an IO expander (ADR-0001).
 *
 * Implemented in Rust, in components/kernel_rs/src/drv_ioexp_xl9555.rs, and
 * exported to loadable drivers through the syscall table under the same name.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Drive a pin of an IO expander a board profile declared, making it an output
 * if the profile did not.
 *
 * For a driver that owns a line — an audio driver gating its own amplifier —
 * and must not depend on a board profile having configured it at boot. The
 * level is latched before the pin becomes an output, so claiming a line never
 * glitches it.
 *
 * Every call is an I²C transaction: call it from a task, never from an
 * interrupt handler.
 *
 * @param expander Index into the board profile's `io_expanders`.
 * @param pin      Pin on that expander, numbered 0-15 across both ports.
 * @param level    Non-zero drives the pin high.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for a negative index or a pin the part
 *         does not have; ESP_ERR_NOT_FOUND for an expander no board profile
 *         declared.
 */
esp_err_t thistle_expander_set_level(int expander, int pin, int level);

#ifdef __cplusplus
}
#endif
