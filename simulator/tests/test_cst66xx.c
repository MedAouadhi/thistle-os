/*
 * Unit tests for the virtual Hynitron CST66xx touch controller.
 *
 * The frame bytes are asserted literally rather than recomputed, so these pin
 * the wire format from LilyGo's hyn_cst66xx.c rather than agreeing with
 * whatever the model happens to build. That distinction is the point: this
 * part was driven for months as a CST328 and as a CST226, and both of those
 * decodes would pass a test that derived its expectation from the model.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "sim_i2c_bus.h"
#include "sim_devices.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdint.h>

#define CST66_ADDR 0x1A

static const uint8_t REG_REPORT[4]   = { 0xD0, 0x07, 0x00, 0x00 };
static const uint8_t REG_INFO[4]     = { 0xD0, 0x03, 0x00, 0x00 };
static const uint8_t CMD_NORMAL_C[4] = { 0xD0, 0x00, 0x01, 0x00 };
static const uint8_t CMD_RELEASE[4]  = { 0xD0, 0x00, 0x02, 0xAB };

static i2c_master_dev_handle_t attach_touch(void)
{
    sim_i2c_bus_init();
    dev_cst66xx_register(0, CST66_ADDR);

    i2c_master_bus_handle_t bus = sim_i2c_bus_get(0);
    i2c_device_config_t cfg = { .device_address = CST66_ADDR };
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_add_device(bus, &cfg, &dev);
    return dev;
}

/* Put the part where a driver leaves it after its probe. */
static i2c_master_dev_handle_t attach_in_normal_mode(void)
{
    i2c_master_dev_handle_t dev = attach_touch();
    i2c_master_transmit(dev, CMD_NORMAL_C, sizeof(CMD_NORMAL_C), 50);
    return dev;
}

static void read_at(i2c_master_dev_handle_t dev, const uint8_t *reg,
                    uint8_t *rx, size_t len)
{
    i2c_master_transmit(dev, reg, 4, 50);
    i2c_master_receive(dev, rx, len, 50);
}

TEST(cst66xx_says_nothing_until_normal_mode_is_selected) {
    /* Three writes select normal reporting and the part is mute before the
     * last of them. A driver that skips the sequence reads zeros, which is
     * what its probe retry exists to survive. */
    i2c_master_dev_handle_t dev = attach_touch();
    ASSERT_TRUE(!dev_cst66xx_in_normal_mode());

    dev_cst66xx_inject_touch(0x123, 0x0AB, true);
    uint8_t rx[9] = {0xFF};
    read_at(dev, REG_REPORT, rx, sizeof(rx));
    for (size_t i = 0; i < sizeof(rx); i++) {
        ASSERT_EQ(rx[i], 0);
    }
}

TEST(cst66xx_reports_a_touch_in_the_documented_frame) {
    i2c_master_dev_handle_t dev = attach_in_normal_mode();
    dev_cst66xx_inject_touch(0x123, 0x0AB, true);

    uint8_t rx[9] = {0};
    read_at(dev, REG_REPORT, rx, sizeof(rx));

    /* [0..1] checksum, little endian: 0x55 + 23 + AB + 40 + 01 + 10 = 0x0174
     * [2]    0xFF, a position report
     * [3]    one finger, no keys
     * [4]    X low eight bits
     * [5]    Y low eight bits
     * [6]    pressure
     * [7]    Y high four in the high nibble, X high four in the low nibble
     * [8]    event 1 (down) in the high nibble, finger id 0 in the low */
    ASSERT_EQ(rx[0], 0x74);
    ASSERT_EQ(rx[1], 0x01);
    ASSERT_EQ(rx[2], 0xFF);
    ASSERT_EQ(rx[3], 0x01);
    ASSERT_EQ(rx[4], 0x23);
    ASSERT_EQ(rx[5], 0xAB);
    ASSERT_EQ(rx[6], 0x40);
    ASSERT_EQ(rx[7], 0x01);
    ASSERT_EQ(rx[8], 0x10);
}

TEST(cst66xx_checksum_covers_the_point_data_seeded_with_0x55) {
    i2c_master_dev_handle_t dev = attach_in_normal_mode();
    dev_cst66xx_inject_touch(0x0C8, 0x190, true);

    uint8_t rx[9] = {0};
    read_at(dev, REG_REPORT, rx, sizeof(rx));

    uint16_t sum = 0x55;
    for (int i = 4; i < 9; i++) {
        sum = (uint16_t)(sum + rx[i]);
    }
    ASSERT_EQ(rx[0], (uint8_t)(sum & 0xFF));
    ASSERT_EQ(rx[1], (uint8_t)(sum >> 8));
}

TEST(cst66xx_reports_a_lift_with_no_fingers) {
    i2c_master_dev_handle_t dev = attach_in_normal_mode();
    dev_cst66xx_inject_touch(100, 200, false);

    uint8_t rx[9] = {0};
    read_at(dev, REG_REPORT, rx, sizeof(rx));

    ASSERT_EQ(rx[2], 0xFF);        /* still a position report */
    ASSERT_EQ(rx[3], 0x00);        /* carrying no fingers */
    ASSERT_EQ(rx[0], 0x55);        /* checksum over nothing is the seed */
    ASSERT_EQ(rx[1], 0x00);
}

TEST(cst66xx_holds_its_report_until_the_host_releases_it) {
    /* A driver that reads without releasing wedges the stream on hardware.
     * Model the latch so that mistake fails here too. */
    i2c_master_dev_handle_t dev = attach_in_normal_mode();
    dev_cst66xx_inject_touch(10, 20, true);
    ASSERT_TRUE(!dev_cst66xx_report_pending());

    uint8_t rx[9] = {0};
    read_at(dev, REG_REPORT, rx, sizeof(rx));
    ASSERT_TRUE(dev_cst66xx_report_pending());

    i2c_master_transmit(dev, CMD_RELEASE, sizeof(CMD_RELEASE), 50);
    ASSERT_TRUE(!dev_cst66xx_report_pending());
}

TEST(cst66xx_ignores_two_byte_addresses_a_cst328_driver_would_send) {
    /* The decisive property. A CST328 driver asks for 0xD000 with two bytes
     * and a CST226 driver asks for 0x00 with one; neither may appear to work,
     * or the simulator would have gone on passing while the board did not. */
    i2c_master_dev_handle_t dev = attach_in_normal_mode();
    dev_cst66xx_inject_touch(0x123, 0x0AB, true);

    const uint8_t cst328_reg[2] = { 0xD0, 0x00 };
    uint8_t rx[9] = {0xFF};
    i2c_master_transmit(dev, cst328_reg, sizeof(cst328_reg), 50);
    i2c_master_receive(dev, rx, sizeof(rx), 50);
    for (size_t i = 0; i < sizeof(rx); i++) {
        ASSERT_EQ(rx[i], 0);
    }

    const uint8_t cst226_reg = 0x00;
    memset(rx, 0xFF, sizeof(rx));
    i2c_master_transmit(dev, &cst226_reg, 1, 50);
    i2c_master_receive(dev, rx, sizeof(rx), 50);
    for (size_t i = 0; i < sizeof(rx); i++) {
        ASSERT_EQ(rx[i], 0);
    }
}

TEST(cst66xx_identifies_itself_the_way_the_real_part_did) {
    /* The values LilyGo's factory firmware printed on the board:
     * ictype caca220e, project 02512010, fw 01010001. The 0xCACA at bytes
     * 2..3 is the signature a driver probes for. */
    i2c_master_dev_handle_t dev = attach_touch();

    uint8_t info[50] = {0};
    read_at(dev, REG_INFO, info, sizeof(info));

    ASSERT_EQ(info[2], 0xCA);
    ASSERT_EQ(info[3], 0xCA);
    ASSERT_EQ(info[1], 0x22);
    ASSERT_EQ(info[0], 0x0E);

    ASSERT_EQ((uint16_t)(info[28] | (info[29] << 8)), 240);
    ASSERT_EQ((uint16_t)(info[30] | (info[31] << 8)), 320);

    ASSERT_EQ(info[32], 0x01);
    ASSERT_EQ(info[35], 0x01);
    ASSERT_EQ(info[36], 0x10);
    ASSERT_EQ(info[39], 0x02);
}
