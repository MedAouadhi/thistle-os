#pragma once

#include "hal/input.h"

const hal_input_driver_t *sim_input_get(void);

/* Call from main loop to pump SDL events -> HAL input events */
void sim_input_poll_sdl(void);

/* Inject one touch edge in native display coordinates. */
bool sim_input_inject_touch(uint16_t x, uint16_t y, bool pressed);

/* Inject a move of a finger that is down. */
bool sim_input_inject_touch_move(uint16_t x, uint16_t y);

/* Inject one key edge, as the keyboard driver reports it. */
bool sim_input_inject_key(uint16_t keycode, bool pressed);
