// SPDX-License-Identifier: BSD-3-Clause
//! The apps the user pinned to Home, place by place, kept in system.json so
//! they come back after a restart:
//!
//! ```json
//! "home": { "pins": ["com.thistle.phone", "com.thistle.reader", "", ...] }
//! ```
//!
//! Home (launcher_ui.c) asks for them each time it is built and saves the
//! whole list when the user changes a place, so the places they did not touch
//! keep their apps. "" is a place never chosen, which Home fills itself, as
//! it does one whose app is no longer installed.

use std::ffi::{c_char, CStr};

use crate::system_json;

const SECTION: &str = "home";
const KEY: &str = "pins";

/// As `HOME_MAX_PINS` in home_core.h.
pub(crate) const MAX_PINS: usize = 8;

const ESP_ERR_INVALID_ARG: i32 = 0x102;

/// The places saved in `json`: at most `MAX_PINS`, anything that is not a
/// string read as "". Empty when there are none.
pub(crate) fn pins_in(json: Option<&str>) -> Vec<String> {
    let Some(value) = json.and_then(|j| system_json::get(j, SECTION, KEY)) else {
        return Vec::new();
    };
    let Some(list) = value.as_array() else {
        return Vec::new();
    };
    list.iter()
        .take(MAX_PINS)
        .map(|v| v.as_str().unwrap_or("").to_string())
        .collect()
}

fn saved() -> Vec<String> {
    let json = std::fs::read_to_string(system_json::path()).ok();
    pins_in(json.as_deref())
}

/// The saved places into `out`, `id_cap` bytes each, at most `max`; how many.
///
/// # Safety
/// `out` must point to `id_cap * max` writable bytes, or be null.
#[no_mangle]
pub unsafe extern "C" fn rs_home_pins_get(out: *mut c_char, id_cap: usize, max: i32) -> i32 {
    if out.is_null() || id_cap == 0 || max <= 0 {
        return 0;
    }
    let pins = saved();
    let n = pins.len().min(max as usize);
    for (i, id) in pins.iter().take(n).enumerate() {
        let dst = out.add(i * id_cap);
        if id.len() + 1 > id_cap || id.as_bytes().contains(&0) {
            *dst = 0;
            continue;
        }
        std::ptr::copy_nonoverlapping(id.as_ptr(), dst as *mut u8, id.len());
        *dst.add(id.len()) = 0;
    }
    n as i32
}

/// Save `n` places from `ids`.
///
/// # Safety
/// `ids` must point to `n` pointers, each null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rs_home_pins_set(ids: *const *const c_char, n: i32) -> i32 {
    if ids.is_null() || n < 0 || n as usize > MAX_PINS {
        return ESP_ERR_INVALID_ARG;
    }
    let list: Vec<serde_json::Value> = (0..n as usize)
        .map(|i| {
            let p = *ids.add(i);
            let id = if p.is_null() {
                ""
            } else {
                CStr::from_ptr(p).to_str().unwrap_or("")
            };
            serde_json::Value::String(id.to_string())
        })
        .collect();
    system_json::save(SECTION, KEY, serde_json::Value::Array(list))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nothing_pinned_until_the_user_pins() {
        assert!(pins_in(None).is_empty());
        assert!(pins_in(Some(r#"{ "thistle_os": {} }"#)).is_empty());
        assert!(pins_in(Some(r#"{ "thistle_os": { "home": { "pins": "x" } } }"#)).is_empty());
    }

    #[test]
    fn reads_the_places_in_order_and_a_place_not_chosen_as_empty() {
        let json = r#"{ "thistle_os": { "home": { "pins": ["com.a", "", 3, "com.b"] } } }"#;
        assert_eq!(pins_in(Some(json)), ["com.a", "", "", "com.b"]);
    }

    #[test]
    fn reads_no_more_than_eight_places() {
        let json = r#"{ "thistle_os": { "home": { "pins": ["1","2","3","4","5","6","7","8","9"] } } }"#;
        assert_eq!(pins_in(Some(json)).len(), MAX_PINS);
    }

    #[test]
    fn set_refuses_more_places_than_home_has() {
        let ids = [std::ptr::null::<c_char>(); MAX_PINS + 1];
        assert_eq!(unsafe { rs_home_pins_set(ids.as_ptr(), ids.len() as i32) }, ESP_ERR_INVALID_ARG);
        assert_eq!(unsafe { rs_home_pins_set(std::ptr::null(), 1) }, ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn get_writes_nothing_into_no_buffer() {
        assert_eq!(unsafe { rs_home_pins_get(std::ptr::null_mut(), 64, 6) }, 0);
    }
}
