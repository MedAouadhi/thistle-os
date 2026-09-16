/*
 * Hynitron CST66xx capacitive touch controller — virtual I2C device model.
 *
 * The part fitted to the T-Deck Pro Max. Modelled to the wire format in
 * LilyGo's lib/HynTouch/src/hyn_cst66xx.c, because a model that answers
 * anything a driver asks for cannot catch a driver that asks wrongly — and
 * three successive touch drivers here spoke a register map this part does not
 * have. So four-byte addresses only, a checksum on every report, and a report
 * the host must release before another appears.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sim_i2c_bus.h"
#include "sim_devices.h"
#include <string.h>
#include <stdio.h>

/* ── Wire constants, from hyn_cst66xx.c ─────────────────────────────────── */

#define CST66_REG_REPORT    0xD0070000u  /* report block                      */
#define CST66_REG_INFO      0xD0030000u  /* 50-byte configuration block       */
#define CST66_CMD_WAKE      0xD0000400u  /* leave low-power I2C mode          */
#define CST66_CMD_NORMAL_A  0xD0000000u
#define CST66_CMD_NORMAL_B  0xD0000C00u
#define CST66_CMD_NORMAL_C  0xD0000100u
#define CST66_CMD_RELEASE   0xD00002ABu  /* report consumed, produce the next */

#define CST66_HEADER_LEN    9
#define CST66_POINT_LEN     5
#define CST66_INFO_LEN      50
#define CST66_CHECKSUM_SEED 0x55
#define CST66_TYPE_POSITION 0xFF

/* What the real part on the T-Deck Max reports about itself, as LilyGo's
 * factory firmware printed it: ictype caca220e, project 02512010, fw 01010001.
 * The 0xCACA is the signature a driver probes for. */
#define CST66_IC_TYPE       0xCACA220Eu
#define CST66_PROJECT_ID    0x02512010u
#define CST66_FW_VERSION    0x01010001u
#define CST66_RES_X         240
#define CST66_RES_Y         320

typedef struct {
    uint16_t touch_x;
    uint16_t touch_y;
    bool     touch_down;
    /* The part holds a report until the host writes the release command. A
     * driver that forgets stalls on real hardware, so it must stall here. */
    bool     report_pending;
    /* Set once the normal-mode sequence has been sent. Before that the part
     * has nothing to say, which is what the driver's probe retry is for. */
    bool     normal_mode;
    /* The address the last write selected. A driver reads this part in two
     * transactions — address, STOP, then a bare read — so by the time the
     * read arrives the address is no longer on the wire, exactly as on
     * hardware where it lives in the chip's own pointer. */
    uint32_t pending_reg;
} cst66xx_model_t;

static cst66xx_model_t cst66xx_model;

static uint16_t cst66xx_checksum(const uint8_t *points, size_t len)
{
    uint16_t sum = CST66_CHECKSUM_SEED;
    for (size_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + points[i]);
    }
    return sum;
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

/* Build the report block for the current state. One finger at most: the HAL
 * event model carries one point, so a second would never be read. */
static void build_report(const cst66xx_model_t *m, uint8_t *out)
{
    memset(out, 0, CST66_HEADER_LEN);
    out[2] = CST66_TYPE_POSITION;
    out[3] = m->touch_down ? 0x01 : 0x00;  /* low nibble fingers, high keys */

    if (m->touch_down) {
        out[4] = (uint8_t)(m->touch_x & 0xFF);
        out[5] = (uint8_t)(m->touch_y & 0xFF);
        out[6] = 0x40;  /* pressure */
        out[7] = (uint8_t)(((m->touch_y >> 4) & 0xF0) | ((m->touch_x >> 8) & 0x0F));
        out[8] = 0x10;  /* event nibble 1 = down, id 0 */
    }

    uint16_t sum = cst66xx_checksum(&out[4], m->touch_down ? CST66_POINT_LEN : 0);
    out[0] = (uint8_t)(sum & 0xFF);
    out[1] = (uint8_t)(sum >> 8);
}

static void build_info(uint8_t *out)
{
    memset(out, 0, CST66_INFO_LEN);
    put_le32(&out[0], CST66_IC_TYPE);      /* bytes 2..3 carry the 0xCACA */
    out[28] = (uint8_t)(CST66_RES_X & 0xFF);
    out[29] = (uint8_t)(CST66_RES_X >> 8);
    out[30] = (uint8_t)(CST66_RES_Y & 0xFF);
    out[31] = (uint8_t)(CST66_RES_Y >> 8);
    put_le32(&out[32], CST66_FW_VERSION);
    put_le32(&out[36], CST66_PROJECT_ID);
    out[48] = 18;  /* tx channels */
    out[49] = 12;  /* rx channels */
}

static esp_err_t cst66xx_serve(cst66xx_model_t *m, uint32_t reg,
                               uint8_t *rx, size_t rx_len)
{
    if (reg == CST66_REG_INFO) {
        uint8_t info[CST66_INFO_LEN];
        build_info(info);
        size_t copy = rx_len < sizeof(info) ? rx_len : sizeof(info);
        memcpy(rx, info, copy);
        memset(rx + copy, 0, rx_len - copy);
        return ESP_OK;
    }

    if (reg == CST66_REG_REPORT) {
        memset(rx, 0, rx_len);
        if (!m->normal_mode) {
            return ESP_OK;
        }
        uint8_t report[CST66_HEADER_LEN];
        build_report(m, report);
        size_t copy = rx_len < sizeof(report) ? rx_len : sizeof(report);
        memcpy(rx, report, copy);
        m->report_pending = true;
        return ESP_OK;
    }

    memset(rx, 0, rx_len);
    return ESP_OK;
}

static esp_err_t cst66xx_on_read(sim_i2c_device_t *dev,
                                 const uint8_t *tx, size_t tx_len,
                                 uint8_t *rx, size_t rx_len)
{
    cst66xx_model_t *m = (cst66xx_model_t *)dev->model;

    /* A bare read serves whatever the last write selected: either the block a
     * four-byte address asked for, or nothing if that write was not one. */
    if (tx_len == 0) {
        return cst66xx_serve(m, m->pending_reg, rx, rx_len);
    }

    /* Four bytes, big endian. Anything else is a driver written for one of
     * this part's cousins, and must not appear to work. */
    if (tx_len != 4) {
        memset(rx, 0, rx_len);
        return ESP_OK;
    }

    uint32_t reg = ((uint32_t)tx[0] << 24) | ((uint32_t)tx[1] << 16)
                 | ((uint32_t)tx[2] << 8)  | (uint32_t)tx[3];
    return cst66xx_serve(m, reg, rx, rx_len);
}

static esp_err_t cst66xx_on_write(sim_i2c_device_t *dev,
                                  const uint8_t *buf, size_t len)
{
    cst66xx_model_t *m = (cst66xx_model_t *)dev->model;

    if (len != 4) {
        /* A wrong-width address selects nothing, so the bare read that
         * follows it cannot be answered by whatever was selected before. */
        m->pending_reg = 0;
        return ESP_OK;
    }

    uint32_t cmd = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                 | ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];

    /* Register addresses and commands share this shape; the value tells them
     * apart, as it does on the part. */
    if (cmd == CST66_REG_REPORT || cmd == CST66_REG_INFO) {
        m->pending_reg = cmd;
        return ESP_OK;
    }
    m->pending_reg = 0;

    switch (cmd) {
    case CST66_CMD_NORMAL_C:
        /* Last of the three that select normal reporting. */
        m->normal_mode = true;
        break;
    case CST66_CMD_RELEASE:
        m->report_pending = false;
        break;
    case CST66_CMD_WAKE:
    case CST66_CMD_NORMAL_A:
    case CST66_CMD_NORMAL_B:
        break;
    default:
        break;
    }
    return ESP_OK;
}

static const sim_i2c_device_ops_t cst66xx_ops = {
    .on_read  = cst66xx_on_read,
    .on_write = cst66xx_on_write,
};

void dev_cst66xx_register(int bus_index, uint16_t addr)
{
    memset(&cst66xx_model, 0, sizeof(cst66xx_model));

    sim_i2c_bus_add_model(bus_index, addr, &cst66xx_ops, &cst66xx_model);
    printf("[sim] CST66xx touch registered on I2C bus %d addr 0x%02X\n",
           bus_index, addr);
}

void dev_cst66xx_inject_touch(uint16_t x, uint16_t y, bool down)
{
    cst66xx_model.touch_x    = x;
    cst66xx_model.touch_y    = y;
    cst66xx_model.touch_down = down;
}

bool dev_cst66xx_report_pending(void)
{
    return cst66xx_model.report_pending;
}

bool dev_cst66xx_in_normal_mode(void)
{
    return cst66xx_model.normal_mode;
}
