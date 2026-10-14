// SPDX-License-Identifier: BSD-3-Clause
//! Whether the GPS receiver is being read, decided in one place.
//!
//! The HAL registry starts the reader at boot, so GPS is on unless the user
//! switched it off. That choice is kept in system.json as
//! `thistle_os.gps.enabled` and put back once the drivers are up. The quick panel and Settings switch
//! it here; an app that asks for GPS through the `thistle_gps_enable`
//! syscall switches it on without saving, so the panel still shows the truth.

use std::sync::atomic::{AtomicBool, Ordering};

use crate::hal_registry::registry;

const SECTION: &str = "gps";
const KEY: &str = "enabled";

const ESP_OK: i32 = 0;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

#[cfg(not(test))]
static TAG: &[u8] = b"gps_manager\0";

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
macro_rules! gps_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}
#[cfg(test)]
macro_rules! gps_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => { let _ = ($level, $($arg,)*); };
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

/// Whether the reader is running, as far as anything here switched it.
static ON: AtomicBool = AtomicBool::new(false);

/// The saved choice: on unless switched off.
pub(crate) fn enabled_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, KEY))
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}

/// Start or stop the reader. `ESP_ERR_NOT_SUPPORTED` on a board with no GPS.
fn apply(on: bool) -> i32 {
    let reg = registry();
    if reg.gps.is_null() {
        return ESP_ERR_NOT_SUPPORTED;
    }
    let drv = unsafe { &*reg.gps };
    let switch = if on { drv.enable } else { drv.disable };
    let ret = match switch {
        Some(f) => unsafe { f() },
        None => ESP_ERR_NOT_SUPPORTED,
    };
    if ret == ESP_OK {
        ON.store(on, Ordering::Relaxed);
    }
    ret
}

/// An app's request for GPS: switched on, not saved.
pub(crate) fn enable_for_app() -> i32 {
    apply(true)
}

/// Put back the choice saved in system.json. kernel_init() calls this once
/// the drivers have started, and with them the reader: switching on a reader
/// that is running only records that it is.
pub(crate) fn restore() {
    if registry().gps.is_null() {
        return;
    }
    let json = std::fs::read_to_string(crate::system_json::path()).ok();
    let on = enabled_in(json.as_deref());
    let ret = apply(on);
    if ret != ESP_OK {
        gps_log!(ESP_LOG_WARN, b"GPS would not switch %s (0x%x)\n\0",
                 if on { b"on\0".as_ptr() } else { b"off\0".as_ptr() }, ret);
    } else if !on {
        gps_log!(ESP_LOG_INFO, b"GPS off, as saved\n\0");
    }
}

/// Whether the board has a GPS receiver.
#[no_mangle]
pub extern "C" fn rs_gps_available() -> bool {
    !registry().gps.is_null()
}

/// Whether the receiver is being read.
#[no_mangle]
pub extern "C" fn rs_gps_is_enabled() -> bool {
    ON.load(Ordering::Relaxed)
}

/// Switch GPS on or off and remember it. The driver's error if it would not
/// switch; otherwise whether the choice was saved.
#[no_mangle]
pub extern "C" fn rs_gps_set_enabled(on: bool) -> i32 {
    let ret = apply(on);
    if ret != ESP_OK {
        return ret;
    }
    gps_log!(ESP_LOG_INFO, b"GPS %s\n\0", if on { b"on\0".as_ptr() } else { b"off\0".as_ptr() });
    crate::system_json::save(SECTION, KEY, serde_json::Value::Bool(on))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn on_when_nothing_is_saved() {
        // As the HAL registry starts it at boot.
        assert!(enabled_in(None));
        assert!(enabled_in(Some(r#"{ "thistle_os": {} }"#)));
        assert!(enabled_in(Some(r#"{ "thistle_os": { "gps": {} } }"#)));
    }

    #[test]
    fn reads_the_saved_choice() {
        assert!(enabled_in(Some(r#"{ "thistle_os": { "gps": { "enabled": true } } }"#)));
        assert!(!enabled_in(Some(r#"{ "thistle_os": { "gps": { "enabled": false } } }"#)));
    }

    #[test]
    fn a_choice_that_is_not_a_bool_reads_as_on() {
        assert!(enabled_in(Some(r#"{ "thistle_os": { "gps": { "enabled": "no" } } }"#)));
        assert!(enabled_in(Some("not json")));
    }

    #[test]
    fn nothing_to_switch_without_a_receiver() {
        // The test registry has no GPS driver.
        if registry().gps.is_null() {
            assert_eq!(apply(true), ESP_ERR_NOT_SUPPORTED);
            assert!(!rs_gps_is_enabled());
        }
    }
}
