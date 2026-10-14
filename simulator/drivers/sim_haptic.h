// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Simulator — fake vibration motor, which prints what it plays
#pragma once

#include "hal/haptic.h"

const hal_haptic_driver_t *sim_haptic_get(void);
