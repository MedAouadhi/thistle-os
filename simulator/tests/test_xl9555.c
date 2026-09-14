/*
 * Unit tests for the virtual XL9555 IO expander and its write log.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "sim_i2c_bus.h"
#include "sim_devices.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdint.h>

#define XL9555_ADDR     0x20
#define REG_INPUT_0     0x00
#define REG_OUTPUT_0    0x02
#define REG_CONFIG_0    0x06

/* Bring up the expander on the virtual bus with an empty write log. */
static i2c_master_dev_handle_t attach_expander(void)
{
    sim_i2c_bus_init();
    dev_xl9555_register(0, XL9555_ADDR);

    i2c_master_bus_handle_t bus = sim_i2c_bus_get(0);
    i2c_device_config_t cfg = { .device_address = XL9555_ADDR };
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_add_device(bus, &cfg, &dev);

    dev_xl9555_clear_log();
    return dev;
}

static uint8_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t value = 0;
    i2c_master_transmit_receive(dev, &reg, 1, &value, 1, 50);
    return value;
}

static void write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    i2c_master_transmit(dev, buf, sizeof(buf), 50);
}

/*
 * Drive one pin as the kernel's expander phase does — configure it as an
 * output, then set its level — so the tests below exercise the model against
 * the traffic it will see in the simulator.
 */
static void raise_rail(i2c_master_dev_handle_t dev, uint8_t pin, int level)
{
    uint8_t port = pin / 8;
    uint8_t mask = (uint8_t)(1 << (pin % 8));

    uint8_t config = read_reg(dev, (uint8_t)(REG_CONFIG_0 + port));
    write_reg(dev, (uint8_t)(REG_CONFIG_0 + port), (uint8_t)(config & ~mask));

    uint8_t output = read_reg(dev, (uint8_t)(REG_OUTPUT_0 + port));
    write_reg(dev, (uint8_t)(REG_OUTPUT_0 + port),
              level ? (uint8_t)(output | mask) : (uint8_t)(output & ~mask));
}

TEST(xl9555_resets_to_all_inputs_with_its_latches_high) {
    i2c_master_dev_handle_t dev = attach_expander();

    ASSERT_EQ(read_reg(dev, REG_CONFIG_0), 0xFF);
    ASSERT_EQ(read_reg(dev, REG_CONFIG_0 + 1), 0xFF);
    ASSERT_EQ(read_reg(dev, REG_OUTPUT_0), 0xFF);
    ASSERT_EQ(dev_xl9555_directions(), 0xFFFF);
}

TEST(xl9555_records_every_write_in_order) {
    i2c_master_dev_handle_t dev = attach_expander();

    write_reg(dev, REG_CONFIG_0, 0xFE);
    write_reg(dev, REG_OUTPUT_0, 0x01);

    ASSERT_EQ(dev_xl9555_write_count(), 2);
    ASSERT_EQ(dev_xl9555_write_at(0)->reg, REG_CONFIG_0);
    ASSERT_EQ(dev_xl9555_write_at(0)->value, 0xFE);
    ASSERT_EQ(dev_xl9555_write_at(1)->reg, REG_OUTPUT_0);
    ASSERT_EQ(dev_xl9555_write_at(1)->value, 0x01);
    ASSERT_EQ(dev_xl9555_writes_dropped(), 0);
}

TEST(xl9555_log_distinguishes_configuration_writes_from_output_writes) {
    i2c_master_dev_handle_t dev = attach_expander();

    raise_rail(dev, 2, 1);   /* port 0 */
    raise_rail(dev, 11, 1);  /* port 1 */

    /* Both ports are represented, and a reader can tell which register each
     * write landed in — that is what makes the log worth asserting against. */
    ASSERT_EQ(dev_xl9555_write_count(), 4);
    ASSERT_EQ(dev_xl9555_write_at(0)->reg, REG_CONFIG_0);
    ASSERT_EQ(dev_xl9555_write_at(1)->reg, REG_OUTPUT_0);
    ASSERT_EQ(dev_xl9555_write_at(2)->reg, REG_CONFIG_0 + 1);
    ASSERT_EQ(dev_xl9555_write_at(3)->reg, REG_OUTPUT_0 + 1);
}

TEST(xl9555_raising_one_rail_leaves_the_rest_of_the_port_alone) {
    i2c_master_dev_handle_t dev = attach_expander();

    raise_rail(dev, 3, 0);

    /* Pin 3 is now an output held low; every other pin is still an input. */
    ASSERT_EQ(dev_xl9555_directions(), 0xFFF7);
    ASSERT_EQ(dev_xl9555_outputs(), 0xFFF7);
}

TEST(xl9555_reads_back_a_driven_pin_and_floats_the_rest_high) {
    i2c_master_dev_handle_t dev = attach_expander();

    raise_rail(dev, 0, 0);

    /* An output reads back at the level it is driven; inputs float high. */
    ASSERT_EQ(read_reg(dev, REG_INPUT_0), 0xFE);
}

TEST(xl9555_never_writes_its_input_ports) {
    i2c_master_dev_handle_t dev = attach_expander();

    write_reg(dev, REG_INPUT_0, 0x00);

    ASSERT_EQ(dev_xl9555_write_count(), 0);
    ASSERT_EQ(read_reg(dev, REG_INPUT_0), 0xFF);
}
