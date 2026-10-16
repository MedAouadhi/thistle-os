// SPDX-License-Identifier: BSD-3-Clause
//! The language dictation is transcribed in, kept in system.json so the
//! user's choice in the quick panel comes back after a reboot:
//!
//! ```json
//! "dictation": { "language": "en" }
//! ```
//!
//! Cohere's transcription takes the language it is told and detects none of
//! its own. Told Arabic, it may answer English speech in Arabic, translated;
//! so the user says which they are speaking. None chosen is Arabic, as the
//! Hermes agent's transcription is. The quick panel switches between English
//! and Arabic; any other code the model knows can be written into the file.

use std::ffi::{c_char, CStr};
use std::sync::Mutex;

use crate::system_json;

const SECTION: &str = "dictation";
const KEY: &str = "language";

/// What dictation is in when nobody has chosen.
pub(crate) const DEFAULT_LANGUAGE: &str = "ar";

const ESP_ERR_INVALID_ARG: i32 = 0x102;

/// The language in use, read from system.json the first time it is asked
/// for: the quick panel asks every second while it is open.
static LANGUAGE: Mutex<Option<String>> = Mutex::new(None);

/// Whether `code` can be sent as a language: an ISO 639-1 code, perhaps with
/// a region, as "en" or "pt-BR". Anything else would break the form it goes
/// in, or be refused.
pub(crate) fn valid(code: &str) -> bool {
    (2..=15).contains(&code.len())
        && code.bytes().all(|b| b.is_ascii_alphabetic() || b == b'-')
        && code.as_bytes()[0].is_ascii_alphabetic()
}

/// The language saved in `json`, or `None` if there is none or it is not one.
pub(crate) fn language_in(json: Option<&str>) -> Option<String> {
    let code = system_json::get(json?, SECTION, KEY)?;
    let code = code.as_str()?;
    valid(code).then(|| code.to_string())
}

fn current() -> String {
    let mut language = LANGUAGE.lock().unwrap_or_else(|e| e.into_inner());
    language
        .get_or_insert_with(|| {
            let json = std::fs::read_to_string(system_json::path()).ok();
            language_in(json.as_deref()).unwrap_or_else(|| DEFAULT_LANGUAGE.to_string())
        })
        .clone()
}

/// The language to dictate in, NUL-terminated into `out`. False if `out`
/// cannot hold it, with "" there when it holds anything.
///
/// # Safety
/// `out` must point to `cap` writable bytes, or be null with `cap` 0.
#[no_mangle]
pub unsafe extern "C" fn rs_dictation_get_language(out: *mut c_char, cap: usize) -> bool {
    if out.is_null() || cap == 0 {
        return false;
    }
    let code = current();
    if code.len() + 1 > cap {
        *out = 0;
        return false;
    }
    std::ptr::copy_nonoverlapping(code.as_ptr(), out as *mut u8, code.len());
    *out.add(code.len()) = 0;
    true
}

/// Dictate in `code` from now on, and save it. ESP_ERR_INVALID_ARG for
/// something that is not a language; otherwise whether it was saved, the
/// choice standing either way until a restart.
///
/// # Safety
/// `code` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rs_dictation_set_language(code: *const c_char) -> i32 {
    if code.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Ok(code) = CStr::from_ptr(code).to_str() else {
        return ESP_ERR_INVALID_ARG;
    };
    if !valid(code) {
        return ESP_ERR_INVALID_ARG;
    }
    *LANGUAGE.lock().unwrap_or_else(|e| e.into_inner()) = Some(code.to_string());
    system_json::save(SECTION, KEY, serde_json::Value::String(code.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arabic_until_someone_chooses() {
        assert_eq!(language_in(None), None);
        assert_eq!(language_in(Some(r#"{ "thistle_os": {} }"#)), None);
        assert_eq!(DEFAULT_LANGUAGE, "ar");
    }

    #[test]
    fn reads_the_language_chosen() {
        let json = r#"{ "thistle_os": { "dictation": { "language": "en" } } }"#;
        assert_eq!(language_in(Some(json)).as_deref(), Some("en"));
    }

    #[test]
    fn a_saved_value_that_is_not_a_language_is_ignored() {
        for bad in [r#""""#, r#""e""#, "3", r#""en\r\n--b""#, r#""a\"b""#, r#""-en""#] {
            let json = format!(r#"{{ "thistle_os": {{ "dictation": {{ "language": {bad} }} }} }}"#);
            assert_eq!(language_in(Some(&json)), None, "{bad}");
        }
    }

    #[test]
    fn a_language_is_letters_and_a_region() {
        assert!(valid("en"));
        assert!(valid("pt-BR"));
        assert!(!valid("e"));
        assert!(!valid("en us"));
        assert!(!valid("ar\""));
        assert!(!valid("abcdefghijklmnop"));
    }

    #[test]
    fn something_that_is_not_a_language_is_refused_before_it_is_kept() {
        let before = current();
        let bad = c"en\r\n";
        assert_eq!(unsafe { rs_dictation_set_language(bad.as_ptr()) }, ESP_ERR_INVALID_ARG);
        assert_eq!(unsafe { rs_dictation_set_language(std::ptr::null()) }, ESP_ERR_INVALID_ARG);
        assert_eq!(current(), before);
    }

    #[test]
    fn get_refuses_a_buffer_too_small() {
        let mut out = [1 as c_char; 2];
        assert!(!unsafe { rs_dictation_get_language(out.as_mut_ptr(), out.len()) });
        assert_eq!(out[0], 0);
        assert!(!unsafe { rs_dictation_get_language(std::ptr::null_mut(), 0) });
        let mut out = [0 as c_char; 16];
        assert!(unsafe { rs_dictation_get_language(out.as_mut_ptr(), out.len()) });
    }
}
