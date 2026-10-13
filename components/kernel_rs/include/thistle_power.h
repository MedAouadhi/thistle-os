/*
 * Keeping the device awake, for a driver that runs something sleep would
 * break. The same two calls as thistle/power_manager.h, for code that sees
 * kernel_rs but not the kernel component.
 *
 * Implemented in Rust, in components/kernel_rs/src/power_manager.rs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Keep the device awake until power_manager_release(). Neither call blocks;
 * both may be made from any task, never from an interrupt.
 *
 * @param why What runs, as the end of "Not sleeping: ..." ("audio is in
 *            use"). Must outlive the hold: a string literal.
 * @return A token for power_manager_release(), never 0.
 */
uint32_t power_manager_hold(const char *why);

/** Let the device sleep again, as far as this hold goes. 0 is ignored. */
void power_manager_release(uint32_t token);

#ifdef __cplusplus
}
#endif
