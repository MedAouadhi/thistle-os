// SPDX-License-Identifier: BSD-3-Clause
//! Reading and updating single settings in system.json without losing the
//! rest of the file — the WiFi credentials live there too.
//!
//! A setting is addressed as `section.key` under `thistle_os`, or under the
//! root in a file that has no `thistle_os` wrapper; those are the two shapes
//! the WiFi credentials have always been read from.

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_FAIL: i32 = -1;

#[cfg(not(test))]
static TAG: &[u8] = b"system_json\0";

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
const ESP_LOG_WARN: i32 = 2;

#[cfg(not(test))]
macro_rules! warn {
    ($fmt:expr) => {
        unsafe { esp_log_write(ESP_LOG_WARN, TAG.as_ptr(), $fmt.as_ptr()) }
    };
}
#[cfg(test)]
macro_rules! warn {
    ($fmt:expr) => {
        let _ = $fmt;
    };
}

/// The system.json in use: the SD card's if it has one, SPIFFS's otherwise.
pub(crate) fn path() -> &'static str {
    #[cfg(target_os = "espidf")]
    {
        if std::path::Path::new("/sdcard/config/system.json").exists() {
            return "/sdcard/config/system.json";
        }
        "/spiffs/config/system.json"
    }
    #[cfg(not(target_os = "espidf"))]
    {
        "/tmp/thistle_sdcard/config/system.json"
    }
}

fn scope(root: &serde_json::Value) -> &serde_json::Value {
    root.get("thistle_os").unwrap_or(root)
}

/// The value at `section.key`, or `None` if the file does not parse or does
/// not have it.
pub(crate) fn get(json: &str, section: &str, key: &str) -> Option<serde_json::Value> {
    let root: serde_json::Value = serde_json::from_str(json).ok()?;
    scope(&root).get(section)?.get(key).cloned()
}

/// `json` with `section.key` set to `value`, creating the section if the file
/// has none. `None` for a file that does not parse, or whose section is
/// something other than an object: rewriting either would lose whatever else
/// the file holds.
pub(crate) fn with_value(
    json: &str,
    section: &str,
    key: &str,
    value: serde_json::Value,
) -> Option<String> {
    let mut root: serde_json::Value = serde_json::from_str(json).ok()?;
    let scope = if root.get("thistle_os").is_some() {
        root.get_mut("thistle_os")?
    } else {
        &mut root
    };
    let section = scope
        .as_object_mut()?
        .entry(section.to_string())
        .or_insert_with(|| serde_json::Value::Object(serde_json::Map::new()))
        .as_object_mut()?;
    section.insert(key.to_string(), value);
    serde_json::to_string_pretty(&root).ok()
}

/// What a device that has no system.json yet starts one from.
pub(crate) const FRESH: &str = r#"{ "thistle_os": {} }"#;

/// A file holding nothing but this one setting, for a device that has no
/// system.json yet.
fn fresh_with_value(section: &str, key: &str, value: serde_json::Value) -> String {
    with_value(FRESH, section, key, value).expect("a fresh system.json always takes a setting")
}

/// Store `value` at `section.key` in the system.json in use, creating the
/// file if there is none.
pub(crate) fn save(section: &str, key: &str, value: serde_json::Value) -> i32 {
    let path = path();
    let updated = match std::fs::read_to_string(path) {
        Ok(json) => {
            // Settings are saved on every tap. Rewriting a file that already
            // says this costs a flash write and risks the rest of it for
            // nothing.
            if get(&json, section, key).as_ref() == Some(&value) {
                return ESP_OK;
            }
            match with_value(&json, section, key, value) {
                Some(updated) => updated,
                None => {
                    warn!(b"system.json is not valid JSON; setting not saved\0");
                    return ESP_ERR_INVALID_STATE;
                }
            }
        }
        Err(_) => fresh_with_value(section, key, value),
    };
    match std::fs::write(path, updated) {
        Ok(()) => ESP_OK,
        Err(_) => {
            warn!(b"could not write system.json; setting not saved\0");
            ESP_FAIL
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const SHIPPED: &str = r#"{
    "thistle_os": {
        "theme": "default",
        "display": { "refresh_mode": "fast" },
        "wifi": { "enabled": true, "ssid": "home", "password": "secret" }
    }
}"#;

    #[test]
    fn test_with_value_sets_one_setting_and_keeps_the_rest() {
        let updated = with_value(SHIPPED, "display", "light", json!(60)).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&updated).unwrap();
        let os = &parsed["thistle_os"];
        assert_eq!(os["display"]["light"], json!(60));
        assert_eq!(os["display"]["refresh_mode"], json!("fast"));
        assert_eq!(os["wifi"]["ssid"], json!("home"));
        assert_eq!(os["wifi"]["password"], json!("secret"));
        assert_eq!(os["theme"], json!("default"));
    }

    #[test]
    fn test_with_value_replaces_an_existing_setting() {
        let updated = with_value(SHIPPED, "display", "refresh_mode", json!("auto")).unwrap();
        assert_eq!(get(&updated, "display", "refresh_mode"), Some(json!("auto")));
    }

    #[test]
    fn test_with_value_creates_a_missing_section() {
        let updated = with_value(SHIPPED, "keyboard", "light", json!(25)).unwrap();
        assert_eq!(get(&updated, "keyboard", "light"), Some(json!(25)));
    }

    #[test]
    fn test_with_value_writes_into_a_file_without_the_wrapper() {
        let updated = with_value(r#"{ "wifi": {} }"#, "display", "light", json!(100)).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&updated).unwrap();
        assert_eq!(parsed["display"]["light"], json!(100));
        assert!(parsed.get("thistle_os").is_none());
    }

    #[test]
    fn test_with_value_refuses_malformed_json() {
        assert_eq!(with_value("{ not json", "display", "light", json!(60)), None);
    }

    #[test]
    fn test_with_value_refuses_a_section_that_is_not_an_object() {
        let json = r#"{ "thistle_os": { "display": "lcd" } }"#;
        assert_eq!(with_value(json, "display", "light", json!(60)), None);
    }

    #[test]
    fn test_get_absent_section_or_key() {
        assert_eq!(get(SHIPPED, "keyboard", "light"), None);
        assert_eq!(get(SHIPPED, "display", "light"), None);
        assert_eq!(get("{ not json", "display", "light"), None);
    }

    #[test]
    fn test_fresh_file_holds_only_the_setting() {
        let json = fresh_with_value("display", "light", json!(25));
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["thistle_os"]["display"]["light"], json!(25));
        assert_eq!(parsed["thistle_os"].as_object().unwrap().len(), 1);
    }
}
