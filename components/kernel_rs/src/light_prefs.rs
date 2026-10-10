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
use crate::wifi_manager::system_json_path;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_FAIL: i32 = -1;

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

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

/// The object a section hangs off: `thistle_os` in the shipped file, or the
/// root itself in a file that has no such wrapper — the same two shapes the
/// WiFi credentials are read from.
fn scope(root: &serde_json::Value) -> &serde_json::Value {
    root.get("thistle_os").unwrap_or(root)
}

/// The level saved under `section`, or `None` if there is none or it is not
/// a percent.
fn read_level(json: &str, section: &str) -> Option<u8> {
    let root: serde_json::Value = serde_json::from_str(json).ok()?;
    let level = scope(&root).get(section)?.get(LEVEL_KEY)?.as_u64()?;
    if level > 100 {
        return None;
    }
    Some(level as u8)
}

/// `json` with `section`'s level set to `percent`, creating the section if
/// the file has none. `None` for a file that does not parse, or whose
/// section is something other than an object — rewriting either would lose
/// whatever else the file holds, the WiFi credentials among it.
fn with_level(json: &str, section: &str, percent: u8) -> Option<String> {
    let mut root: serde_json::Value = serde_json::from_str(json).ok()?;
    let scope = if root.get("thistle_os").is_some() {
        root.get_mut("thistle_os")?
    } else {
        &mut root
    };
    let scope = scope.as_object_mut()?;
    let section = scope
        .entry(section.to_string())
        .or_insert_with(|| serde_json::Value::Object(serde_json::Map::new()))
        .as_object_mut()?;
    section.insert(LEVEL_KEY.to_string(), serde_json::Value::from(percent));
    serde_json::to_string_pretty(&root).ok()
}

/// A file holding nothing but this one level, for a device that has no
/// system.json yet.
fn fresh_with_level(section: &str, percent: u8) -> String {
    with_level(r#"{ "thistle_os": {} }"#, section, percent)
        .expect("a fresh system.json always takes a level")
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
    let path = system_json_path();
    let updated = match std::fs::read_to_string(path) {
        Ok(json) => {
            // Every tap lands here. Rewriting a file that already says this
            // costs a flash write and risks the rest of it for nothing.
            if read_level(&json, section) == Some(percent) {
                return ESP_OK;
            }
            match with_level(&json, section, percent) {
                Some(updated) => updated,
                None => {
                    light_log!(ESP_LOG_WARN, b"system.json is not valid JSON; level not saved\0");
                    return ESP_ERR_INVALID_STATE;
                }
            }
        }
        Err(_) => fresh_with_level(section, percent),
    };
    match std::fs::write(path, updated) {
        Ok(()) => ESP_OK,
        Err(_) => {
            light_log!(ESP_LOG_WARN, b"could not write system.json; level not saved\0");
            ESP_FAIL
        }
    }
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
    let path = system_json_path();
    let Ok(json) = std::fs::read_to_string(path) else {
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
        "display": { "refresh_mode": "partial" },
        "wifi": { "enabled": true, "ssid": "home", "password": "secret" }
    }
}"#;

    #[test]
    fn test_with_level_sets_display_light_and_keeps_the_rest() {
        let updated = with_level(SHIPPED, DISPLAY_SECTION, 60).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&updated).unwrap();
        let os = &parsed["thistle_os"];
        assert_eq!(os["display"]["light"].as_u64(), Some(60));
        assert_eq!(os["display"]["refresh_mode"].as_str(), Some("partial"));
        assert_eq!(os["wifi"]["ssid"].as_str(), Some("home"));
        assert_eq!(os["wifi"]["password"].as_str(), Some("secret"));
        assert_eq!(os["theme"].as_str(), Some("default"));
    }

    #[test]
    fn test_with_level_creates_a_missing_section() {
        let updated = with_level(SHIPPED, KEYBOARD_SECTION, 25).unwrap();
        assert_eq!(read_level(&updated, KEYBOARD_SECTION), Some(25));
    }

    #[test]
    fn test_with_level_writes_into_a_file_without_the_wrapper() {
        let updated = with_level(r#"{ "wifi": {} }"#, DISPLAY_SECTION, 100).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&updated).unwrap();
        assert_eq!(parsed["display"]["light"].as_u64(), Some(100));
        assert!(parsed.get("thistle_os").is_none());
    }

    #[test]
    fn test_with_level_refuses_malformed_json() {
        assert_eq!(with_level("{ not json", DISPLAY_SECTION, 60), None);
    }

    #[test]
    fn test_with_level_refuses_a_section_that_is_not_an_object() {
        let json = r#"{ "thistle_os": { "display": "lcd" } }"#;
        assert_eq!(with_level(json, DISPLAY_SECTION, 60), None);
    }

    #[test]
    fn test_off_is_saved_as_a_level() {
        let updated = with_level(SHIPPED, DISPLAY_SECTION, 0).unwrap();
        assert_eq!(read_level(&updated, DISPLAY_SECTION), Some(0));
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
    fn test_fresh_file_holds_only_the_level() {
        let json = fresh_with_level(DISPLAY_SECTION, 25);
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["thistle_os"]["display"]["light"].as_u64(), Some(25));
        assert_eq!(parsed["thistle_os"].as_object().unwrap().len(), 1);
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
