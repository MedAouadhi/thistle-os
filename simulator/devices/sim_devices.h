/*
 * Simulator device models — register on virtual I2C/SPI bus.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Register a device model on the virtual I2C bus */
void dev_pcf8563_register(int bus_index, uint16_t addr);
void dev_qmi8658c_register(int bus_index, uint16_t addr);
void dev_tca8418_register(int bus_index, uint16_t addr);
void dev_cst328_register(int bus_index, uint16_t addr);
void dev_ltr553_register(int bus_index, uint16_t addr);
void dev_xl9555_register(int bus_index, uint16_t addr);

/* ── XL9555 IO expander write log ───────────────────────────────────────────
 * The expander records every register write, so the expander phase of a board
 * profile's boot sequence is inspectable: which register, with what value, in
 * what order. Configuration registers are 0x06/0x07, output registers
 * 0x02/0x03, and `0 = output, 1 = input` throughout. */
#define DEV_XL9555_MAX_WRITES 64

typedef struct {
    uint8_t reg;
    uint8_t value;
} dev_xl9555_write_t;

int dev_xl9555_write_count(void);
int dev_xl9555_writes_dropped(void);
const dev_xl9555_write_t *dev_xl9555_write_at(int index);
void dev_xl9555_clear_log(void);
/* Pin levels and directions as the expander currently holds them, port 0 in
 * the low byte. A set direction bit is an input, as the part resets to. */
uint16_t dev_xl9555_outputs(void);
uint16_t dev_xl9555_directions(void);

/* Input injection (called from SDL event handler) */
void dev_tca8418_inject_key(uint8_t keycode, bool press);
void dev_cst328_inject_touch(uint16_t x, uint16_t y, bool down);

/* Sensor value injection */
void dev_qmi8658c_set_accel(float x, float y, float z);
void dev_qmi8658c_set_gyro(float x, float y, float z);
void dev_ltr553_set_lux(uint16_t lux);
void dev_ltr553_set_proximity(uint16_t prox);
