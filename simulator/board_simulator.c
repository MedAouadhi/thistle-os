#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "hal/board.h"
#include "sim_display.h"
#include "sim_input.h"
#include "sim_storage.h"
#include "sim_power.h"
#include "sim_gps.h"
#include "sim_imu.h"
#include "sim_haptic.h"
#include "sim_rtc.h"
#include "sim_audio.h"
#include "sim_radio.h"
#include "sim_scenario.h"
#include "sim_i2c_bus.h"
#include "sim_spi_bus.h"
#include "sim_devices.h"

/* Forward declaration — defined in sim_network.c */
esp_err_t sim_network_register(void);
esp_err_t sim_cellular_register(void);

/* Selected device — set by main() via sim_board_set_device() before board_init() */
static const char *s_device = "tdeck";

void sim_board_set_device(const char *device)
{
    s_device = device;
}

typedef struct {
    const char *name;
    const char *board_name;
    int         width;
    int         height;
    bool        has_keyboard;
    bool        has_touch;
    /* The Max's touch part is a CST66xx; the Pro and the rest are CST328.
     * They share neither register map nor address width. */
    bool        touch_is_cst66xx;
    bool        has_radio;
    bool        has_gps;
    bool        is_epaper;
    /* An XL9555 on the I²C bus, carrying the peripheral rails and the
     * keyboard and touch resets. */
    bool        has_io_expander;
    /* A board that carries neither part leaves both off, which keeps the
     * simulator honest about what an app can detect there. */
    bool        has_rtc;
    bool        has_light_sensor;
    /* A cellular modem: the A7682E of the Max's profile. */
    bool        has_modem;
    /* A vibration motor: the DRV2605 of the Max's profile. */
    bool        has_haptic;
} sim_device_t;

/* Every device has an RTC and a light sensor unless it says otherwise, which
 * is why those two are named here and the rest of the capabilities are not. */
#define SIM_DEVICE_DEFAULTS  .has_rtc = true, .has_light_sensor = true

static const sim_device_t DEVICES[] = {
    { .name = "tdeck-pro",  .board_name = "T-Deck Pro (Simulator)",    .width = 240, .height = 320,
      .has_keyboard = true, .has_touch = true, .has_radio = true, .has_gps = true,
      .is_epaper = true,  SIM_DEVICE_DEFAULTS },
    { .name = "tdeck",      .board_name = "T-Deck (Simulator)",        .width = 320, .height = 240,
      .has_keyboard = true, .has_touch = true, .has_radio = true, .has_gps = true,
      SIM_DEVICE_DEFAULTS },
    { .name = "tdeck-plus", .board_name = "T-Deck Plus (Simulator)",   .width = 320, .height = 240,
      .has_keyboard = true, .has_touch = true, .has_radio = true, .has_gps = true,
      SIM_DEVICE_DEFAULTS },
    /* The Max is the only device with an IO expander, and the only one with
     * neither an RTC nor a light sensor — it carries neither part. */
    { .name = "tdeck-max",  .board_name = "T-Deck Pro Max (Simulator)", .width = 240, .height = 320,
      .has_keyboard = true, .has_touch = true, .touch_is_cst66xx = true,
      .has_radio = true, .has_gps = true,
      .is_epaper = true, .has_io_expander = true, .has_modem = true, .has_haptic = true },
    { .name = "tdisplay",   .board_name = "T-Display-S3 (Simulator)",  .width = 320, .height = 170,
      .has_touch = true,  SIM_DEVICE_DEFAULTS },
    { .name = "heltec-v3",  .board_name = "Heltec V3 (Simulator)",     .width = 128, .height = 64,
      .has_radio = true,  SIM_DEVICE_DEFAULTS },
    { .name = "cardputer",  .board_name = "Cardputer (Simulator)",     .width = 240, .height = 135,
      .has_keyboard = true,  SIM_DEVICE_DEFAULTS },
    { .name = "t3-s3",      .board_name = "T3-S3 (Simulator)",         .width = 128, .height = 64,
      .has_radio = true,  SIM_DEVICE_DEFAULTS },
    { .name = "rak3312",    .board_name = "RAK3312 (Simulator)",       .width = 128, .height = 64,
      .has_radio = true,  SIM_DEVICE_DEFAULTS },
    { .name = NULL },
};

static const sim_device_t *find_device(const char *name)
{
    for (int i = 0; DEVICES[i].name != NULL; i++) {
        if (strcmp(DEVICES[i].name, name) == 0) return &DEVICES[i];
    }
    /* Default: tdeck (index 1) */
    return &DEVICES[1];
}

esp_err_t board_init(void)
{
    const sim_device_t *dev = find_device(s_device);
    printf("Simulator: %s (%dx%d)\n", dev->board_name, dev->width, dev->height);

    sim_display_set_resolution(dev->width, dev->height);
    sim_display_set_epaper(dev->is_epaper);
    sim_display_set_title(dev->board_name);

    hal_set_board_name(dev->board_name);
    hal_display_register(sim_display_get(), NULL);
    hal_input_register(sim_input_get(), NULL);
    hal_storage_register(sim_storage_get(), NULL);

    /* Register host network transport — net_manager_init() is called
     * by kernel_init() before driver_manager_init(), so the manager is
     * already initialized when board_init() runs. */
    sim_network_register();
    if (dev->has_modem) sim_cellular_register();

    /* Register fake HAL drivers so apps never see NULL vtables */
    hal_power_register(sim_power_get(), NULL);
    /* A board with no RTC part leaves the interface unregistered and falls
     * back to the system tick, exactly as it does on hardware. */
    if (dev->has_rtc) hal_rtc_register(sim_rtc_get());
    hal_imu_register(sim_imu_get(), NULL);
    hal_audio_register(sim_audio_get(), NULL);
    if (dev->has_radio) hal_radio_register(sim_radio_get(), NULL);
    if (dev->has_gps)   hal_gps_register(sim_gps_get(), NULL);
    if (dev->has_haptic) hal_haptic_register(sim_haptic_get(), NULL);

    /* Initialize virtual buses and register device models */
    sim_i2c_bus_init();
    sim_spi_bus_init();
    hal_bus_register_i2c(0, sim_i2c_bus_get(0));
    hal_bus_register_spi(2, sim_spi_bus_get(0));

    /* Register I2C device models based on device capabilities */
    if (dev->has_rtc)          dev_pcf8563_register(0, 0x51);
    if (dev->has_keyboard)     dev_tca8418_register(0, 0x34);
    if (dev->has_touch) {
        if (dev->touch_is_cst66xx) dev_cst66xx_register(0, 0x1A);
        else                       dev_cst328_register(0, 0x1A);
    }
    if (dev->has_io_expander)  dev_xl9555_register(0, 0x20);
    dev_qmi8658c_register(0, 0x6A);    /* IMU — all devices with accel */
    if (dev->has_light_sensor) dev_ltr553_register(0, 0x23);

    /* Apply scenario state to fake drivers */
    {
        uint16_t mv; uint8_t pct; int pstate;
        sim_scenario_get_power(&mv, &pct, &pstate);
        sim_power_set(mv, pct, (hal_power_state_t)pstate);
    }
    if (dev->has_gps) {
        double lat, lon; float alt; uint8_t sats; bool fix;
        sim_scenario_get_gps(&lat, &lon, &alt, &sats, &fix);
        sim_gps_set_position(lat, lon, alt, sats, fix);
    }
    {
        float accel[3], gyro[3];
        sim_scenario_get_imu(accel, gyro);
        sim_imu_set_accel(accel[0], accel[1], accel[2]);
        sim_imu_set_gyro(gyro[0], gyro[1], gyro[2]);
    }

    return ESP_OK;
}

/* Query device capabilities from main.c for conditional app loading */
bool sim_board_has_radio(void) { return find_device(s_device)->has_radio; }
bool sim_board_has_gps(void) { return find_device(s_device)->has_gps; }
bool sim_board_has_keyboard(void) { return find_device(s_device)->has_keyboard; }
bool sim_board_has_touch(void) { return find_device(s_device)->has_touch; }
bool sim_board_is_epaper(void) { return find_device(s_device)->is_epaper; }
