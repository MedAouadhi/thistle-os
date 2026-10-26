// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Kernel boot sequence

use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::Mutex;

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;

// ── ESP-IDF error codes needed for boot ──────────────────────────────
const ESP_ERR_NVS_NO_FREE_PAGES: i32 = 0x1101;
const ESP_ERR_NVS_NEW_VERSION_FOUND: i32 = 0x1102;

// ESP-IDF FFI
extern "C" {
    fn esp_timer_get_time() -> i64;
}

// ── SPIFFS FFI (hardware builds only) ────────────────────────────────

#[cfg(target_os = "espidf")]
#[repr(C)]
struct SpiffsConf {
    base_path: *const u8,
    partition_label: *const u8,
    max_files: usize,
    format_if_mount_failed: bool,
}

#[cfg(target_os = "espidf")]
extern "C" {
    fn esp_vfs_spiffs_register(conf: *const SpiffsConf) -> i32;
    fn esp_spiffs_info(partition: *const u8, total: *mut usize, used: *mut usize) -> i32;
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

/// Mount the SPIFFS filesystem at /spiffs using the "storage" partition.
///
/// This replaces the C `spiffs_mount()` shim in kernel_shims.c.
fn spiffs_mount() -> i32 {
    #[cfg(target_os = "espidf")]
    unsafe {
        let conf = SpiffsConf {
            base_path: b"/spiffs\0".as_ptr(),
            partition_label: b"storage\0".as_ptr(),
            max_files: 10,
            format_if_mount_failed: true,
        };
        let ret = esp_vfs_spiffs_register(&conf);
        if ret == ESP_OK {
            let mut total: usize = 0;
            let mut used: usize = 0;
            esp_spiffs_info(b"storage\0".as_ptr(), &mut total, &mut used);
            esp_log_write(
                3, /* INFO */
                b"spiffs\0".as_ptr(),
                b"Mounted /spiffs (total: %u, used: %u)\0".as_ptr(),
                total as u32,
                used as u32,
            );
        } else {
            esp_log_write(
                1, /* ERROR */
                b"spiffs\0".as_ptr(),
                b"Mount failed: %d\0".as_ptr(),
                ret,
            );
        }
        return ret;
    }
    #[cfg(not(target_os = "espidf"))]
    ESP_OK
}

// ── NVS flash FFI (hardware builds only) ─────────────────────────────

#[cfg(target_os = "espidf")]
extern "C" {
    fn nvs_flash_init() -> i32;
    fn nvs_flash_erase() -> i32;
}

/// Initialise NVS flash, erasing and retrying on version mismatch.
///
/// This replaces the C `nvs_flash_init_safe()` shim in kernel_shims.c.
fn nvs_flash_init_safe() -> i32 {
    #[cfg(target_os = "espidf")]
    unsafe {
        let mut ret = nvs_flash_init();
        if ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND {
            nvs_flash_erase();
            ret = nvs_flash_init();
        }
        return ret;
    }
    #[cfg(not(target_os = "espidf"))]
    ESP_OK
}

// Subsystem init functions (some Rust, some still C)
extern "C" {
    fn net_manager_init() -> i32;
    fn driver_manager_init() -> i32;
    fn driver_manager_start_all() -> i32;
    fn syscall_table_init() -> i32;
    fn board_config_init(path: *const c_char) -> i32;
    fn board_init() -> i32;
    fn driver_loader_init() -> i32;
    fn driver_loader_scan_and_load() -> i32;
    fn elf_loader_init() -> i32;
    fn ota_init() -> i32;
    fn drv_crypto_mbedtls_get() -> *const std::os::raw::c_void;
    fn hal_crypto_register(driver: *const std::os::raw::c_void) -> i32;
    fn wifi_manager_init() -> i32;
    fn wifi_manager_auto_connect() -> i32;
    fn net_manager_register_wifi();
}

static BOOT_TIME_US: Mutex<i64> = Mutex::new(0);

/// The public half of the publisher's Ed25519 release key, made 2026-10-26.
/// Its private half exists only as the secret RELEASE_SIGNING_KEY of the
/// fork's GitHub `release` environment, which admits release tags alone
/// (ADR-0011). tools/production_public.key holds the same 32 bytes. It
/// replaced upstream's production key; every build, laptop or released, is a
/// cargo release build and embeds it.
const RELEASE_KEY: [u8; 32] = [
    0xac, 0x87, 0x97, 0xa9, 0x11, 0x77, 0xf7, 0x94,
    0x6a, 0x13, 0x30, 0xa1, 0x4f, 0x9d, 0x48, 0x4c,
    0xb7, 0xfd, 0x02, 0x4d, 0x08, 0xc1, 0x3d, 0x20,
    0x8a, 0x04, 0x84, 0x6a, 0x88, 0xdb, 0xdd, 0x3e,
];

// kernel_init, kernel_uptime_ms — same names as C

#[no_mangle]
pub extern "C" fn kernel_init() -> i32 {
    if let Ok(mut t) = BOOT_TIME_US.lock() {
        *t = unsafe { esp_timer_get_time() };
    }

    // The kernel worker, before any driver that runs jobs on it starts. The
    // browser simulator has no threads, so there the worker cannot start and
    // its jobs never run, as the threads they replaced never started there.
    if !crate::kworker::start() && !cfg!(target_os = "emscripten") {
        return ESP_FAIL;
    }

    // NVS flash — required by WiFi, BLE, and other ESP-IDF subsystems
    let ret = nvs_flash_init_safe();
    if ret != ESP_OK {
        return ret;
    }

    // Mount SPIFFS — apps, drivers, config, themes live here
    let ret = spiffs_mount();
    if ret != ESP_OK {
        // Non-fatal: SD card can still provide apps/config
    }

    // Event bus (Rust)
    let ret = crate::event::event_bus_init();
    if ret != ESP_OK {
        return ret;
    }

    // IPC (Rust)
    let ret = crate::ipc::ipc_init();
    if ret != ESP_OK {
        return ret;
    }

    // Network manager (C)
    let ret = unsafe { net_manager_init() };
    if ret != ESP_OK {
        return ret;
    }

    // Syscall table (C for now)
    let ret = unsafe { syscall_table_init() };
    if ret != ESP_OK {
        return ret;
    }

    // Register mbedtls hardware-accelerated crypto driver (board-independent)
    unsafe {
        hal_crypto_register(drv_crypto_mbedtls_get());
    }

    // Board config: reads board.json, inits buses, loads drivers
    let ret = unsafe { board_config_init(std::ptr::null()) };
    if ret != ESP_OK {
        // Fallback to compiled board_init + driver_manager
        unsafe {
            driver_manager_init();
            driver_manager_start_all();
            driver_loader_init();
            driver_loader_scan_and_load();
        }
    }

    // The screen and key lights the user last chose. Here, once the drivers
    // are up and the SD card that may hold system.json is mounted.
    crate::light_prefs::light_prefs_restore();

    // GPS, unless the user switched it off.
    crate::gps_manager::restore();

    // The volume, and whether the device was left muted.
    crate::audio_volume::restore();

    // The LoRa antenna the user last chose. Boot left the radio on the
    // internal one, and nothing has transmitted yet.
    crate::radio_antenna::radio_antenna_restore();

    // The MeshCore node, on whichever antenna that left selected. It runs from
    // boot so messages arrive whether or not an app is open; system.json can
    // switch it off. A board with no radio skips it.
    crate::mesh_manager::mesh_start();

    // App manager (Rust)
    crate::app_manager::app_manager_init();

    // Register the thistle-tk launcher (always register — main.c decides
    // whether to launch it based on the active WM). The other thistle-tk
    // apps wait for tk_native_apps_register(): see there.
    crate::tk_launcher::register();

    // Permissions (Rust)
    crate::permissions::init();

    // Signing (Rust): the kernel's key is the publisher's release key, in
    // every build. Firmware, drivers and window managers take it alone.
    unsafe {
        crate::signing::signing_init(RELEASE_KEY.as_ptr());
    }
    // A key pair made on the build machine, which tools/make_card_appstore.py
    // signs its packages with; kernel_rs/CMakeLists.txt passes its public
    // half in. Trusted for apps only.
    if let Some(key) = option_env!("THISTLE_APP_PUBLISHER_KEY").and_then(crate::signing::parse_key_hex) {
        if crate::signing::add_app_publisher(&key) == ESP_OK {
            #[cfg(target_os = "espidf")]
            unsafe {
                esp_log_write(
                    3, /* INFO */
                    b"signing\0".as_ptr(),
                    b"Apps signed by this build's publisher key %02x%02x%02x%02x... are trusted too\0"
                        .as_ptr(),
                    key[0] as u32,
                    key[1] as u32,
                    key[2] as u32,
                    key[3] as u32,
                );
            }
        }
    }

    // ELF loader (C)
    let ret = unsafe { elf_loader_init() };
    if ret != ESP_OK {
        return ret;
    }

    // OTA (C)
    let ret = unsafe { ota_init() };
    if ret != ESP_OK {
        return ret;
    }

    // WiFi (C, non-fatal)
    let ret = unsafe { wifi_manager_init() };
    if ret == ESP_OK {
        unsafe {
            net_manager_register_wifi();
        }
        // Auto-connect to saved WiFi credentials from system.json
        unsafe {
            wifi_manager_auto_connect();
        }
    }

    // The clock, once a clock chip would be registered and esp_netif is up:
    // the zone, the chip's time if the clock lost it, and NTP.
    crate::time_manager::start();

    // From here on the network coordinator keeps WiFi and mobile data up as
    // the user set them; esp_netif routes over WiFi whenever it is up.
    crate::net_coordinator::start();

    // Calls: a kernel worker job reads the modem's RI line once a second,
    // and a call ringing starts the call thread. Only on a board whose modem
    // makes calls, now registered with the drivers.
    crate::call_manager::start();

    // The tailnet, if the user has it on: a kernel worker job joins it once
    // there is a network and the clock is set (tailnet_manager.rs).
    crate::tailnet_manager::start();

    // AssistNow: orbits fetched once a day and handed to a u-blox receiver
    // as it starts, if a token has been given (gps_assist.rs).
    crate::gps_assist::start();

    // Publish SYSTEM_BOOT event
    crate::event::event_publish_simple(0); // EVENT_SYSTEM_BOOT = 0

    ESP_OK
}

/// Register the thistle-tk App Store and Flashlight. Call this only when
/// thistle-tk is the window manager.
///
/// Each shares its app ID with an LVGL counterpart, and the app manager keeps
/// whichever registers first. Registering these from kernel_init() put them
/// ahead of the LVGL built-ins on every board, so under an LVGL manager the
/// LVGL App Store and Flashlight could never be launched.
#[no_mangle]
pub extern "C" fn tk_native_apps_register() -> i32 {
    let rc = crate::tk_appstore::register();
    let fl = crate::tk_flashlight::register();
    if rc != ESP_OK {
        rc
    } else {
        fl
    }
}

#[no_mangle]
pub extern "C" fn kernel_uptime_ms() -> u32 {
    let boot = BOOT_TIME_US.lock().map(|t| *t).unwrap_or(0);
    let now = unsafe { esp_timer_get_time() };
    ((now - boot) / 1000).min(u32::MAX as i64) as u32
}

#[cfg(test)]
mod tests {
    use super::RELEASE_KEY;

    // The public half of the publisher's release key, whose private half is
    // the secret RELEASE_SIGNING_KEY of the fork's `release` environment.
    const RELEASE_KEY_HEX: &str = "ac8797a91177f7946a1330a14f9d484cb7fd024d08c13d208a04846a88dbdd3e";

    #[test]
    fn the_kernel_trusts_the_release_key() {
        assert_eq!(crate::signing::parse_key_hex(RELEASE_KEY_HEX), Some(RELEASE_KEY));
    }

    #[test]
    fn the_tracked_public_key_file_is_the_kernels_key() {
        // tools/production_public.key is what `tools/sign.py verify` and the
        // release workflow check a release's manifest against.
        let tracked: &[u8] = include_bytes!("../../../tools/production_public.key");
        assert_eq!(tracked, RELEASE_KEY.as_slice());
    }
}
