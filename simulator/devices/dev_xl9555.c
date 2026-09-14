/*
 * XL9555 16-bit I2C IO expander — virtual device model.
 *
 * Records every register write in order, so that a board profile's expander
 * phase can be inspected without a flash cycle: what was written, in what
 * order, and what state the part was left in.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sim_i2c_bus.h"
#include "sim_devices.h"
#include <string.h>
#include <stdio.h>

/* Register map — PCA9535-compatible, two ports of eight pins each. */
#define REG_INPUT_0    0x00
#define REG_INPUT_1    0x01
#define REG_OUTPUT_0   0x02
#define REG_OUTPUT_1   0x03
#define REG_POLARITY_0 0x04
#define REG_POLARITY_1 0x05
#define REG_CONFIG_0   0x06
#define REG_CONFIG_1   0x07
#define XL9555_NUM_REGS 8

typedef struct {
    uint8_t             regs[XL9555_NUM_REGS];
    dev_xl9555_write_t  writes[DEV_XL9555_MAX_WRITES];
    int                 write_count;
    /* Writes attempted past the log's capacity, so a test can tell a full
     * log from a complete one. */
    int                 writes_dropped;
} xl9555_model_t;

static xl9555_model_t xl9555_model;

/* Pins configured as outputs read back as the level they are driven at; pins
 * left as inputs float high, as the board's pull-ups leave them. */
static uint8_t xl9555_input_port(const xl9555_model_t *m, int port)
{
    uint8_t config = m->regs[REG_CONFIG_0 + port];
    uint8_t output = m->regs[REG_OUTPUT_0 + port];
    return (uint8_t)((output & (uint8_t)~config) | config);
}

static esp_err_t xl9555_on_read(sim_i2c_device_t *dev,
                                const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t rx_len)
{
    xl9555_model_t *m = (xl9555_model_t *)dev->model;
    if (tx_len < 1) { memset(rx, 0, rx_len); return ESP_OK; }

    m->regs[REG_INPUT_0] = xl9555_input_port(m, 0);
    m->regs[REG_INPUT_1] = xl9555_input_port(m, 1);

    /* Auto-increment read, wrapping within the eight-register window. */
    for (size_t i = 0; i < rx_len; i++) {
        rx[i] = m->regs[(tx[0] + i) % XL9555_NUM_REGS];
    }
    return ESP_OK;
}

static esp_err_t xl9555_on_write(sim_i2c_device_t *dev,
                                 const uint8_t *buf, size_t len)
{
    xl9555_model_t *m = (xl9555_model_t *)dev->model;
    if (len < 2) return ESP_OK;

    for (size_t i = 1; i < len; i++) {
        uint8_t reg = (uint8_t)((buf[0] + (i - 1)) % XL9555_NUM_REGS);
        /* The input ports are read-only — the pins drive them. */
        if (reg == REG_INPUT_0 || reg == REG_INPUT_1) continue;
        m->regs[reg] = buf[i];

        if (m->write_count < DEV_XL9555_MAX_WRITES) {
            m->writes[m->write_count].reg   = reg;
            m->writes[m->write_count].value = buf[i];
            m->write_count++;
        } else {
            m->writes_dropped++;
        }
    }
    return ESP_OK;
}

static const sim_i2c_device_ops_t xl9555_ops = {
    .on_read  = xl9555_on_read,
    .on_write = xl9555_on_write,
};

void dev_xl9555_register(int bus_index, uint16_t addr)
{
    memset(&xl9555_model, 0, sizeof(xl9555_model));

    /* Reset state: every pin an input, every output latch high. A board that
     * sets a level without setting direction leaves its rail off, and this is
     * the state that makes that visible. */
    xl9555_model.regs[REG_CONFIG_0] = 0xFF;
    xl9555_model.regs[REG_CONFIG_1] = 0xFF;
    xl9555_model.regs[REG_OUTPUT_0] = 0xFF;
    xl9555_model.regs[REG_OUTPUT_1] = 0xFF;

    sim_i2c_bus_add_model(bus_index, addr, &xl9555_ops, &xl9555_model);
    printf("[sim] XL9555 IO expander registered on I2C bus %d addr 0x%02X\n",
           bus_index, addr);
}

int dev_xl9555_write_count(void)
{
    return xl9555_model.write_count;
}

int dev_xl9555_writes_dropped(void)
{
    return xl9555_model.writes_dropped;
}

const dev_xl9555_write_t *dev_xl9555_write_at(int index)
{
    if (index < 0 || index >= xl9555_model.write_count) return NULL;
    return &xl9555_model.writes[index];
}

void dev_xl9555_clear_log(void)
{
    xl9555_model.write_count = 0;
    xl9555_model.writes_dropped = 0;
}

uint16_t dev_xl9555_outputs(void)
{
    return (uint16_t)(xl9555_model.regs[REG_OUTPUT_0] |
                      (xl9555_model.regs[REG_OUTPUT_1] << 8));
}

uint16_t dev_xl9555_directions(void)
{
    return (uint16_t)(xl9555_model.regs[REG_CONFIG_0] |
                      (xl9555_model.regs[REG_CONFIG_1] << 8));
}
