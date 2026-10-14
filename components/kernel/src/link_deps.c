// SPDX-License-Identifier: BSD-3-Clause
// Force-link ESP-IDF symbols that the Rust kernel references via FFI.
//
// The Rust staticlib (libthistle_kernel.a) calls these functions directly.
// Without this file, the linker may not pull them in from ESP-IDF component
// archives because the Rust lib is processed before those archives.
//
// This file is never called — it just ensures the symbols are present.

#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#if CONFIG_BT_ENABLED
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#endif
#include "esp_elf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

// esp_ptr_in_drom became `inline static` in ESP-IDF v6 (esp_memory_utils.h),
// which makes it not addressable for FFI. Expose a normal extern wrapper so
// Rust callers can keep using a regular extern "C" declaration.
#include "esp_memory_utils.h"
bool thistle_esp_ptr_in_drom(const void *p)
{
    return esp_ptr_in_drom(p);
}

// WiFi init shim — WIFI_INIT_CONFIG_DEFAULT is a C macro that can't be called
// directly from Rust FFI. This wrapper creates the properly-initialised config
// (including magic values that ESP-IDF v6 validates strictly) and calls
// esp_wifi_init. The zeroed-buffer approach used before broke in v6.
#include "esp_log.h"
static const char *WIFI_SHIM_TAG = "wifi_shim";
esp_err_t thistle_wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Debug: confirm shim is running and osi_funcs is populated.
    // osi_funcs NULL means WIFI_INIT_CONFIG_DEFAULT expanded incorrectly.
    ESP_LOGI(WIFI_SHIM_TAG, "osi_funcs=%p magic=0x%08x",
             (void *)cfg.osi_funcs, (unsigned)cfg.magic);
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_SHIM_TAG, "esp_wifi_init returned 0x%x", ret);
    }
    return ret;
}

// The FreeRTOS name of the threads the calling thread starts from now on, as
// task lists and the debugger show them; NULL for esp_pthread's own,
// "pthread". Rust's std::thread names a thread for Rust alone: esp_pthread
// takes the task name from this per-thread setting. The string must outlive
// the threads.
#include "esp_pthread.h"
esp_err_t thistle_pthread_name_next(const char *name)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    if (name) {
        cfg.thread_name = name;
    }
    return esp_pthread_set_cfg(&cfg);
}

// wifi_manager.rs reads and writes these structs by byte offset. Its
// constants must match the headers; when an ESP-IDF update moves a field,
// the build stops here instead of the scan quietly reading the wrong bytes.
#include <stddef.h>
_Static_assert(sizeof(wifi_ap_record_t) == 92, "AP_RECORD_SIZE");
_Static_assert(offsetof(wifi_ap_record_t, ssid) == 6, "AP_SSID_OFFSET");
_Static_assert(sizeof(((wifi_ap_record_t *)0)->ssid) == 33, "AP_SSID_FIELD_LEN");
_Static_assert(offsetof(wifi_ap_record_t, primary) == 39, "AP_CHANNEL_OFFSET");
_Static_assert(offsetof(wifi_ap_record_t, rssi) == 44, "AP_RSSI_OFFSET");
_Static_assert(offsetof(wifi_ap_record_t, authmode) == 48, "AP_AUTHMODE_OFFSET");
_Static_assert(sizeof(((wifi_ap_record_t *)0)->authmode) == 4, "authmode is read as a u32");
_Static_assert(offsetof(wifi_event_sta_disconnected_t, reason) == 39,
               "STA_DISCONNECTED_REASON_OFFSET");
_Static_assert(sizeof(((wifi_event_sta_disconnected_t *)0)->reason) == 1,
               "reason is read as a u8");
_Static_assert(sizeof(wifi_config_t) <= 256, "wifi_manager_connect's config buffer");
_Static_assert(offsetof(wifi_sta_config_t, password) == 32, "password at offset 32");
_Static_assert(offsetof(wifi_sta_config_t, sae_pwe_h2e) == 136, "STA_SAE_PWE_H2E_OFFSET");
_Static_assert(sizeof(((wifi_sta_config_t *)0)->sae_pwe_h2e) == 4, "sae_pwe_h2e is a u32");
_Static_assert(WPA3_SAE_PWE_BOTH == 3, "WPA3_SAE_PWE_BOTH");

// net_manager.rs and net_coordinator.rs mirror these as #[repr(C)] structs.
#include "thistle/net_manager.h"
_Static_assert(sizeof(hal_cell_info_t) == 40, "CCellInfo size");
_Static_assert(offsetof(hal_cell_info_t, sim) == 4, "CCellInfo.sim");
_Static_assert(offsetof(hal_cell_info_t, reg) == 8, "CCellInfo.reg");
_Static_assert(offsetof(hal_cell_info_t, rssi_dbm) == 12, "CCellInfo.rssi_dbm");
_Static_assert(offsetof(hal_cell_info_t, operator_name) == 14, "CCellInfo.operator_name");
_Static_assert(sizeof(hal_net_state_t) == 4 && sizeof(hal_sim_state_t) == 4,
               "C enums are read as u32");
_Static_assert(offsetof(net_cellular_status_t, data) == 4, "CCellularStatus.data");
_Static_assert(offsetof(net_cellular_status_t, sim_blocked) == 8, "CCellularStatus.sim_blocked");
_Static_assert(offsetof(net_cellular_status_t, info) == 12, "CCellularStatus.info");
_Static_assert(offsetof(net_cellular_status_t, sms) == 52, "CCellularStatus.sms");
_Static_assert(sizeof(net_cellular_status_t) == 56, "CCellularStatus size");
_Static_assert(offsetof(hal_net_driver_t, get_cell_info) == 9 * sizeof(void *),
               "HalNetDriver.get_cell_info");
_Static_assert(offsetof(hal_net_driver_t, sms_fetch) == 15 * sizeof(void *),
               "HalNetDriver.sms_fetch");
_Static_assert(offsetof(hal_net_driver_t, get_network_time) == 16 * sizeof(void *),
               "HalNetDriver.get_network_time");
_Static_assert(offsetof(hal_net_driver_t, call_dial) == 17 * sizeof(void *),
               "HalNetDriver.call_dial");
_Static_assert(offsetof(hal_net_driver_t, call_set_speaker) == 25 * sizeof(void *),
               "HalNetDriver.call_set_speaker");
_Static_assert(sizeof(hal_call_state_t) == 4, "CHalCall.state is read as a u32");
_Static_assert(offsetof(hal_call_t, incoming) == 1, "CHalCall.incoming");
_Static_assert(offsetof(hal_call_t, state) == 4, "CHalCall.state");
_Static_assert(offsetof(hal_call_t, number) == 8, "CHalCall.number");
_Static_assert(sizeof(hal_call_t) == 44, "CHalCall size");
_Static_assert(offsetof(hal_net_time_t, age_ms) == 8, "CNetTime.age_ms");
_Static_assert(offsetof(hal_net_time_t, utc_offset_min) == 12, "CNetTime.utc_offset_min");
_Static_assert(offsetof(hal_net_time_t, offset_known) == 14, "CNetTime.offset_known");
_Static_assert(sizeof(hal_net_time_t) == 16, "CNetTime size");

// hal_registry.rs mirrors hal_audio_driver_t as HalAudioDriver.
#include "hal/audio.h"
_Static_assert(offsetof(hal_audio_driver_t, set_route) == 9 * sizeof(void *),
               "HalAudioDriver.set_route");
_Static_assert(sizeof(hal_audio_route_t) == 4, "set_route's route is passed as a u32");

// call_manager.rs, call_log.rs and phonebook.rs fill these in place.
#include "thistle/call.h"
#include "thistle/phonebook.h"
_Static_assert(sizeof(call_state_t) == 4 && sizeof(call_end_t) == 4,
               "CCallStatus enums are read as u32");
_Static_assert(offsetof(call_status_t, incoming) == 8, "CCallStatus.incoming");
_Static_assert(offsetof(call_status_t, name) == 42, "CCallStatus.name");
_Static_assert(offsetof(call_status_t, duration_s) == 108, "CCallStatus.duration_s");
_Static_assert(offsetof(call_status_t, seq) == 116, "CCallStatus.seq");
_Static_assert(sizeof(call_status_t) == 120, "CCallStatus size");
_Static_assert(offsetof(call_log_entry_t, kind) == 33, "CCallLogEntry.kind");
_Static_assert(offsetof(call_log_entry_t, time) == 36, "CCallLogEntry.time");
_Static_assert(sizeof(call_log_entry_t) == 44, "CCallLogEntry size");
_Static_assert(offsetof(pb_contact_t, starred) == 64, "CPbContact.starred");
_Static_assert(offsetof(pb_contact_t, numbers) == 66, "CPbContact.numbers");
_Static_assert(sizeof(pb_contact_t) == 336, "CPbContact size");

// hal_registry.rs mirrors hal_power_driver_t as HalPowerDriver.
#include "hal/power.h"
_Static_assert(offsetof(hal_power_driver_t, name) == 7 * sizeof(void *), "HalPowerDriver.name");
_Static_assert(offsetof(hal_power_driver_t, is_plugged_in) == 8 * sizeof(void *),
               "HalPowerDriver.is_plugged_in");

// time_manager.rs mirrors time_status_t as CTimeStatus.
#include "thistle/time_manager.h"
_Static_assert(sizeof(time_source_t) == 4, "CTimeStatus.source is read as a u32");
_Static_assert(offsetof(time_status_t, source) == 4, "CTimeStatus.source");
_Static_assert(offsetof(time_status_t, synced_at) == 8, "CTimeStatus.synced_at");
_Static_assert(offsetof(time_status_t, has_rtc) == 18, "CTimeStatus.has_rtc");
_Static_assert(offsetof(time_status_t, zone) == 19, "CTimeStatus.zone");
_Static_assert(sizeof(time_status_t) == 72, "CTimeStatus size");

// net_manager.rs mirrors hal_sms_t as CHalSms; sms_manager.rs mirrors
// sms_message_t as CSmsMessage.
#include "thistle/sms.h"
_Static_assert(offsetof(hal_sms_t, sent_at) == 33, "CHalSms.sent_at");
_Static_assert(offsetof(hal_sms_t, text) == 57, "CHalSms.text");
_Static_assert(offsetof(hal_sms_t, concat_ref) == 538, "CHalSms.concat_ref");
_Static_assert(offsetof(hal_sms_t, parts) == 541, "CHalSms.parts");
_Static_assert(sizeof(hal_sms_t) == 542, "CHalSms size");
_Static_assert(offsetof(sms_message_t, text) == 33, "CSmsMessage.text");
_Static_assert(offsetof(sms_message_t, sent) == 1060, "CSmsMessage.sent");
_Static_assert(sizeof(sms_message_t) == 1064, "CSmsMessage size");

// msg_alert_t as CMsgAlert.
#include "thistle/msg_alert.h"
_Static_assert(offsetof(msg_alert_t, transport) == 4, "CMsgAlert.transport");
_Static_assert(offsetof(msg_alert_t, from) == 5, "CMsgAlert.from");
_Static_assert(offsetof(msg_alert_t, text) == 38, "CMsgAlert.text");
_Static_assert(sizeof(msg_alert_t) == 112, "CMsgAlert size");

// Reference each symbol so the linker pulls it from the ESP-IDF archive.
// This function is never called — it's dead code that exists only to
// create linker references.
__attribute__((used))
static void _force_link_deps(void) {
    // SPIFFS
    (void)esp_vfs_spiffs_register;
    (void)esp_spiffs_info;
    // NVS
    (void)nvs_flash_init;
    (void)nvs_flash_erase;
    // BLE / NimBLE
#if CONFIG_BT_ENABLED
    (void)ble_gap_adv_stop;
    (void)ble_gap_terminate;
    (void)ble_svc_gap_device_name_set;
    (void)ble_svc_gap_init;
    (void)ble_svc_gatt_init;
    (void)nimble_port_init;
    (void)nimble_port_freertos_init;
    (void)nimble_port_freertos_deinit;
    (void)nimble_port_run;
    (void)ble_hs_mbuf_from_flat;
    (void)ble_gatts_notify_custom;
#endif
    // ELF loader
    (void)esp_elf_init;
    (void)esp_elf_relocate;
    (void)esp_elf_request;
    (void)esp_elf_deinit;
    // ADC oneshot + calibration (called by drv_power_tp4065b.rs)
    (void)adc_oneshot_new_unit;
    (void)adc_oneshot_del_unit;
    (void)adc_oneshot_config_channel;
    (void)adc_oneshot_read;
    (void)adc_cali_raw_to_voltage;
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    (void)adc_cali_create_scheme_line_fitting;
    (void)adc_cali_delete_scheme_line_fitting;
#endif
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    (void)adc_cali_create_scheme_curve_fitting;
    (void)adc_cali_delete_scheme_curve_fitting;
#endif
    // FreeRTOS (xTaskCreate is a macro wrapping xTaskCreatePinnedToCore)
    (void)xTaskCreatePinnedToCore;
    (void)vTaskDelete;
    (void)vTaskDelay;
    // UART (called by drv_gps_mia_m10q.rs)
    // Note: uart_set_pin is a variadic macro in ESP-IDF v6 — not addressable.
    (void)uart_driver_install;
    (void)uart_driver_delete;
    (void)uart_param_config;
    (void)uart_read_bytes;
    (void)uart_write_bytes;
    // GPIO (called by multiple drivers)
    (void)gpio_set_direction;
    (void)gpio_set_level;
    (void)gpio_get_level;
    (void)gpio_set_pull_mode;
    (void)gpio_isr_handler_add;
    (void)gpio_isr_handler_remove;
    // I2C master (called by keyboard, touch, accel, OLED, IMU, light drivers)
    (void)i2c_master_bus_add_device;
    (void)i2c_master_transmit;
    (void)i2c_master_transmit_receive;
    // SPI (called by e-paper, SD card drivers)
    (void)spi_device_polling_transmit;
}
