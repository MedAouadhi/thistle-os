// SPDX-License-Identifier: BSD-3-Clause
//! The screen's light and the keys' backlight, kept in system.json so the
//! levels the user picked come back after a reboot.
//!
//! Each lives in its own section under `thistle_os`:
//!
//! ```json
//! "display":  { "light": 60 },
//! "keyboard": { "light": 25 }
//! ```
//!
//! A missing level means nobody chose one, and the driver's own default
//! stands. Both drivers on the T-Deck Max start dark on purpose: a reflective
//! panel reads without light, and boot should not spend battery on it.

use crate::hal_registry::{registry, HalDisplayType};
use crate::system_json;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;

const DISPLAY_SECTION: &str = "display";
const KEYBOARD_SECTION: &str = "keyboard";
const LEVEL_KEY: &str = "light";

#[cfg(not(test))]
static TAG: &[u8] = b"light_prefs\0";

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
macro_rules! light_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}
#[cfg(test)]
macro_rules! light_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => { let _ = ($level, $($arg,)*); };
}

const ESP_LOG_INFO: i32 = 3;

/// The level saved under `section`, or `None` if there is none or it is not
/// a percent.
fn read_level(json: &str, section: &str) -> Option<u8> {
    let level = system_json::get(json, section, LEVEL_KEY)?.as_u64()?;
    if level > 100 {
        return None;
    }
    Some(level as u8)
}

/// The level to give the screen at boot. An emissive screen is never put out:
/// with no light it shows nothing, and the user could not see to bring it
/// back. A reflective one still reads without it.
fn display_level_to_restore(saved: Option<u8>, display_type: HalDisplayType) -> Option<u8> {
    match (saved, display_type) {
        (Some(0), HalDisplayType::Lcd) => None,
        (level, _) => level,
    }
}

fn save(section: &str, percent: u8) -> i32 {
    if percent > 100 {
        return ESP_ERR_INVALID_ARG;
    }
    system_json::save(section, LEVEL_KEY, serde_json::Value::from(percent))
}

/// Save the screen's light level, 0-100 percent.
#[no_mangle]
pub extern "C" fn light_prefs_save_display(percent: u8) -> i32 {
    save(DISPLAY_SECTION, percent)
}

/// Save the key backlight level, 0-100 percent.
#[no_mangle]
pub extern "C" fn light_prefs_save_keys(percent: u8) -> i32 {
    save(KEYBOARD_SECTION, percent)
}

/// Give the screen and the keys the levels saved in system.json. Call once the
/// drivers have started; a level nobody saved leaves its driver's default.
#[no_mangle]
pub extern "C" fn light_prefs_restore() {
    let Ok(json) = std::fs::read_to_string(system_json::path()) else {
        return;
    };
    let reg = registry();

    if !reg.display.is_null() {
        let drv = unsafe { &*reg.display };
        let level = display_level_to_restore(read_level(&json, DISPLAY_SECTION), drv.display_type);
        if let (Some(level), Some(set)) = (level, drv.set_brightness) {
            let ret = unsafe { set(level) };
            if ret == ESP_OK {
                light_log!(ESP_LOG_INFO, b"screen light restored to %u%%\0", level as u32);
            }
        }
    }

    if let Some(level) = read_level(&json, KEYBOARD_SECTION) {
        for i in 0..(reg.input_count as usize) {
            if reg.inputs[i].is_null() {
                continue;
            }
            let drv = unsafe { &*reg.inputs[i] };
            if let Some(set) = drv.set_backlight {
                if unsafe { set(level) } == ESP_OK {
                    light_log!(ESP_LOG_INFO, b"key backlight restored to %u%%\0", level as u32);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHIPPED: &str = r#"{
    "thistle_os": {
        "theme": "default",
        "display": { "refresh_mode": "fast" },
        "wifi": { "enabled": true, "ssid": "home", "password": "secret" }
    }
}"#;

    fn saved(section: &str, percent: u8) -> String {
        system_json::with_value(SHIPPED, section, LEVEL_KEY, serde_json::Value::from(percent))
            .unwrap()
    }

    #[test]
    fn test_a_saved_level_reads_back() {
        assert_eq!(read_level(&saved(DISPLAY_SECTION, 60), DISPLAY_SECTION), Some(60));
        assert_eq!(read_level(&saved(KEYBOARD_SECTION, 25), KEYBOARD_SECTION), Some(25));
    }

    #[test]
    fn test_off_is_saved_as_a_level() {
        assert_eq!(read_level(&saved(DISPLAY_SECTION, 0), DISPLAY_SECTION), Some(0));
    }

    #[test]
    fn test_a_level_over_100_is_refused_before_it_reaches_the_file() {
        assert_eq!(save(DISPLAY_SECTION, 101), ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn test_read_level_absent_when_never_saved() {
        assert_eq!(read_level(SHIPPED, DISPLAY_SECTION), None);
        assert_eq!(read_level(SHIPPED, KEYBOARD_SECTION), None);
    }

    #[test]
    fn test_read_level_ignores_the_unrelated_brightness_key() {
        // The shipped file once carried a "brightness" nothing read. It must
        // not light a panel that was meant to start dark.
        let json = r#"{ "thistle_os": { "display": { "brightness": 100 } } }"#;
        assert_eq!(read_level(json, DISPLAY_SECTION), None);
    }

    #[test]
    fn test_read_level_rejects_out_of_range_and_non_numbers() {
        let json = r#"{ "thistle_os": { "display": { "light": 250 },
                                         "keyboard": { "light": "full" } } }"#;
        assert_eq!(read_level(json, DISPLAY_SECTION), None);
        assert_eq!(read_level(json, KEYBOARD_SECTION), None);
    }

    #[test]
    fn test_an_lcd_is_never_restored_dark() {
        assert_eq!(display_level_to_restore(Some(0), HalDisplayType::Lcd), None);
        assert_eq!(display_level_to_restore(Some(25), HalDisplayType::Lcd), Some(25));
    }

    #[test]
    fn test_epaper_is_restored_dark_when_saved_dark() {
        assert_eq!(display_level_to_restore(Some(0), HalDisplayType::Epaper), Some(0));
        assert_eq!(display_level_to_restore(Some(60), HalDisplayType::Epaper), Some(60));
        assert_eq!(display_level_to_restore(None, HalDisplayType::Epaper), None);
    }
}
