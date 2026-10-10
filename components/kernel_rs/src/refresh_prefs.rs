// SPDX-License-Identifier: BSD-3-Clause
//! The e-paper refresh mode picked in Settings, kept in system.json as
//! `thistle_os.display.refresh_mode`: `"fast"`, `"full"` or `"auto"`.
//!
//! The UI manager reads it once at start and applies it to every panel
//! refresh; the panel driver only ever hears full or fast.

use crate::system_json;

const ESP_ERR_INVALID_ARG: i32 = 0x102;

const SECTION: &str = "display";
const KEY: &str = "refresh_mode";

// Mirrors epaper_refresh_policy_t in components/ui/include/ui/epaper_refresh.h.
const POLICY_FAST: u8 = 0;
const POLICY_FULL: u8 = 1;
const POLICY_AUTO: u8 = 2;

fn name_of(policy: u8) -> Option<&'static str> {
    match policy {
        POLICY_FAST => Some("fast"),
        POLICY_FULL => Some("full"),
        POLICY_AUTO => Some("auto"),
        _ => None,
    }
}

/// The policy saved in `json`. Anything else is fast, which is what the panel
/// did before there was a choice — including the `"partial"` the shipped file
/// carried while nothing read it.
fn policy_in(json: &str) -> u8 {
    match system_json::get(json, SECTION, KEY).as_ref().and_then(|v| v.as_str()) {
        Some("full") => POLICY_FULL,
        Some("auto") => POLICY_AUTO,
        _ => POLICY_FAST,
    }
}

/// The refresh policy saved in system.json, or fast if none is.
#[no_mangle]
pub extern "C" fn refresh_prefs_load() -> u8 {
    match std::fs::read_to_string(system_json::path()) {
        Ok(json) => policy_in(&json),
        Err(_) => POLICY_FAST,
    }
}

/// Save the refresh policy, one of epaper_refresh_policy_t's values.
#[no_mangle]
pub extern "C" fn refresh_prefs_save(policy: u8) -> i32 {
    match name_of(policy) {
        Some(name) => system_json::save(SECTION, KEY, serde_json::Value::from(name)),
        None => ESP_ERR_INVALID_ARG,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file_with(mode: &str) -> String {
        format!(r#"{{ "thistle_os": {{ "display": {{ "refresh_mode": "{mode}" }} }} }}"#)
    }

    #[test]
    fn test_each_policy_round_trips_through_its_name() {
        for policy in [POLICY_FAST, POLICY_FULL, POLICY_AUTO] {
            assert_eq!(policy_in(&file_with(name_of(policy).unwrap())), policy);
        }
    }

    #[test]
    fn test_the_old_partial_value_reads_as_fast() {
        assert_eq!(policy_in(&file_with("partial")), POLICY_FAST);
    }

    #[test]
    fn test_no_saved_mode_reads_as_fast() {
        assert_eq!(policy_in(r#"{ "thistle_os": { "display": { "light": 60 } } }"#), POLICY_FAST);
        assert_eq!(policy_in(r#"{ "thistle_os": {} }"#), POLICY_FAST);
        assert_eq!(policy_in("{ not json"), POLICY_FAST);
    }

    #[test]
    fn test_a_mode_that_is_not_a_string_reads_as_fast() {
        let json = r#"{ "thistle_os": { "display": { "refresh_mode": 1 } } }"#;
        assert_eq!(policy_in(json), POLICY_FAST);
    }

    #[test]
    fn test_an_unknown_policy_is_refused_before_it_reaches_the_file() {
        assert_eq!(refresh_prefs_save(3), ESP_ERR_INVALID_ARG);
    }
}
