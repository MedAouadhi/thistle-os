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
//!
//! This module also keeps the level each light is at now, and whether the
//! board has it at all, for the quick panel and Settings, which set the
//! lights through here. A light is taken to be there until its driver answers
//! `ESP_ERR_NOT_SUPPORTED`, which is how a driver says its board left the pin
//! unwired. Boot settles the question by setting each light to its saved
//! level, or to the dark it starts in, which lights nothing.

use std::sync::atomic::{AtomicU8, Ordering};

use crate::hal_registry::{registry, HalDisplayType};
use crate::system_json;

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

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

// ---------------------------------------------------------------------------
// The levels the lights are at now
// ---------------------------------------------------------------------------

const UNKNOWN: u8 = 0;
const YES: u8 = 1;
const NO: u8 = 2;

static DISPLAY_HAS: AtomicU8 = AtomicU8::new(UNKNOWN);
static DISPLAY_LEVEL: AtomicU8 = AtomicU8::new(0);
static KEYS_HAS: AtomicU8 = AtomicU8::new(UNKNOWN);
static KEYS_LEVEL: AtomicU8 = AtomicU8::new(0);
/// The level the keys go back to when switched on: the last they were lit at.
static KEYS_ON_LEVEL: AtomicU8 = AtomicU8::new(DEFAULT_KEYS_ON);

/// The keys' level when switched on with no lit level to go back to:
/// Settings' "Med".
const DEFAULT_KEYS_ON: u8 = 60;

/// The dimmest an emissive screen is offered: at 0 it shows nothing, and the
/// user could not see to bring it back.
const EMISSIVE_MIN: u8 = 5;

/// The lowest level the screen should be offered.
pub(crate) fn display_min(display_type: HalDisplayType) -> u8 {
    match display_type {
        HalDisplayType::Lcd => EMISSIVE_MIN,
        HalDisplayType::Epaper => 0,
    }
}

/// The level a screen is at when nobody saved one: an emissive one comes up
/// lit, a reflective one dark.
fn display_default(display_type: HalDisplayType) -> u8 {
    match display_type {
        HalDisplayType::Lcd => 100,
        HalDisplayType::Epaper => 0,
    }
}

/// The level to set the screen to at boot. A reflective screen with nothing
/// saved is set to the dark it starts in, which asks its driver whether it
/// has a light without lighting it. An emissive one is left as its driver
/// started it.
fn display_level_at_boot(saved: Option<u8>, display_type: HalDisplayType) -> Option<u8> {
    match display_level_to_restore(saved, display_type) {
        None if display_type == HalDisplayType::Epaper => Some(0),
        level => level,
    }
}

/// Whether a light is there, from what its drivers answered. Some answered
/// OK: it is. All that were asked said `NOT_SUPPORTED`, or none could be: it
/// is not. Otherwise a driver failed, which says nothing either way.
fn has_light(any_ok: bool, any_failed: bool) -> u8 {
    if any_ok {
        YES
    } else if any_failed {
        UNKNOWN
    } else {
        NO
    }
}

/// Set the screen's light now, and remember the level it is at.
fn set_display_now(percent: u8) -> i32 {
    let reg = registry();
    let Some(set) = (unsafe { reg.display.as_ref() }).and_then(|d| d.set_brightness) else {
        DISPLAY_HAS.store(NO, Ordering::Relaxed);
        return ESP_ERR_NOT_SUPPORTED;
    };
    let ret = unsafe { set(percent) };
    if ret == ESP_OK {
        DISPLAY_LEVEL.store(percent, Ordering::Relaxed);
    }
    let has = has_light(ret == ESP_OK, ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED);
    if has != UNKNOWN {
        DISPLAY_HAS.store(has, Ordering::Relaxed);
    }
    ret
}

/// Set every key backlight now, and remember the level they are at.
fn set_keys_now(percent: u8) -> i32 {
    let reg = registry();
    let mut any_ok = false;
    let mut any_failed = false;
    for i in 0..(reg.input_count as usize) {
        let Some(set) = (unsafe { reg.inputs[i].as_ref() }).and_then(|d| d.set_backlight) else {
            continue;
        };
        match unsafe { set(percent) } {
            ESP_OK => any_ok = true,
            ESP_ERR_NOT_SUPPORTED => {}
            _ => any_failed = true,
        }
    }
    let has = has_light(any_ok, any_failed);
    if has != UNKNOWN {
        KEYS_HAS.store(has, Ordering::Relaxed);
    }
    if !any_ok {
        return if any_failed { ESP_FAIL } else { ESP_ERR_NOT_SUPPORTED };
    }
    KEYS_LEVEL.store(percent, Ordering::Relaxed);
    if percent > 0 {
        KEYS_ON_LEVEL.store(percent, Ordering::Relaxed);
    }
    ESP_OK
}

/// Whether the board has a screen light.
#[no_mangle]
pub extern "C" fn light_display_available() -> bool {
    let reg = registry();
    let has_setter = unsafe { reg.display.as_ref() }.is_some_and(|d| d.set_brightness.is_some());
    has_setter && DISPLAY_HAS.load(Ordering::Relaxed) != NO
}

/// The screen light's level now, 0-100.
#[no_mangle]
pub extern "C" fn light_display_level() -> u8 {
    DISPLAY_LEVEL.load(Ordering::Relaxed)
}

/// The dimmest the screen should be offered: above 0 on a screen that shows
/// nothing without its light.
#[no_mangle]
pub extern "C" fn light_display_min() -> u8 {
    let reg = registry();
    unsafe { reg.display.as_ref() }.map_or(0, |d| display_min(d.display_type))
}

/// Set the screen's light, and save the level if `save`. The driver's error
/// if it refused (`ESP_ERR_NOT_SUPPORTED` with no light); otherwise whether
/// the level was saved.
#[no_mangle]
pub extern "C" fn light_set_display(percent: u8, save_it: bool) -> i32 {
    if percent > 100 {
        return ESP_ERR_INVALID_ARG;
    }
    let ret = set_display_now(percent);
    if ret != ESP_OK || !save_it {
        return ret;
    }
    light_prefs_save_display(percent)
}

/// Whether the board has a key backlight.
#[no_mangle]
pub extern "C" fn light_keys_available() -> bool {
    let reg = registry();
    let has_setter = (0..reg.input_count as usize)
        .any(|i| unsafe { reg.inputs[i].as_ref() }.is_some_and(|d| d.set_backlight.is_some()));
    has_setter && KEYS_HAS.load(Ordering::Relaxed) != NO
}

/// The key backlight's level now, 0-100.
#[no_mangle]
pub extern "C" fn light_keys_level() -> u8 {
    KEYS_LEVEL.load(Ordering::Relaxed)
}

/// The level the keys go back to when switched on: the last they were lit at.
#[no_mangle]
pub extern "C" fn light_keys_on_level() -> u8 {
    KEYS_ON_LEVEL.load(Ordering::Relaxed)
}

/// Set the key backlight, and save the level if `save`. As for the screen.
#[no_mangle]
pub extern "C" fn light_set_keys(percent: u8, save_it: bool) -> i32 {
    if percent > 100 {
        return ESP_ERR_INVALID_ARG;
    }
    let ret = set_keys_now(percent);
    if ret != ESP_OK || !save_it {
        return ret;
    }
    light_prefs_save_keys(percent)
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
    let json = std::fs::read_to_string(system_json::path()).unwrap_or_default();
    let reg = registry();

    if let Some(drv) = unsafe { reg.display.as_ref() } {
        let saved = read_level(&json, DISPLAY_SECTION);
        DISPLAY_LEVEL.store(display_default(drv.display_type), Ordering::Relaxed);
        if let Some(level) = display_level_at_boot(saved, drv.display_type) {
            if set_display_now(level) == ESP_OK && saved.is_some() {
                light_log!(ESP_LOG_INFO, b"screen light restored to %u%%\0", level as u32);
            }
        }
    }

    // The keys start dark, so with nothing saved they are set dark, which
    // asks their drivers whether they have a light.
    let saved = read_level(&json, KEYBOARD_SECTION);
    if set_keys_now(saved.unwrap_or(0)) == ESP_OK {
        if let Some(level) = saved {
            light_log!(ESP_LOG_INFO, b"key backlight restored to %u%%\0", level as u32);
        }
    }
}

/// The level to give the screen on waking: the saved one, or, on an emissive
/// screen that never had one saved, full. Sleep put its light out, and with
/// no light an LCD shows nothing at all.
fn display_level_after_sleep(saved: Option<u8>, display_type: HalDisplayType) -> Option<u8> {
    match display_level_to_restore(saved, display_type) {
        None if display_type == HalDisplayType::Lcd => Some(100),
        level => level,
    }
}

/// Put back the lights sleep turned off, at the levels they were at before.
/// Sleep put them out through their drivers, not through here, so the levels
/// kept here are the ones to go back to.
pub(crate) fn restore_after_sleep() {
    let reg = registry();
    if let Some(drv) = unsafe { reg.display.as_ref() } {
        let level = Some(DISPLAY_LEVEL.load(Ordering::Relaxed));
        if let Some(level) = display_level_after_sleep(level, drv.display_type) {
            set_display_now(level);
        }
    }
    set_keys_now(KEYS_LEVEL.load(Ordering::Relaxed));
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

    #[test]
    fn after_sleep_an_lcd_with_nothing_saved_comes_back_lit() {
        assert_eq!(display_level_after_sleep(None, HalDisplayType::Lcd), Some(100));
        assert_eq!(display_level_after_sleep(Some(30), HalDisplayType::Lcd), Some(30));
        assert_eq!(display_level_after_sleep(Some(0), HalDisplayType::Lcd), Some(100));
    }

    #[test]
    fn at_boot_epaper_with_nothing_saved_is_set_dark_to_find_its_light() {
        assert_eq!(display_level_at_boot(None, HalDisplayType::Epaper), Some(0));
        assert_eq!(display_level_at_boot(Some(40), HalDisplayType::Epaper), Some(40));
    }

    #[test]
    fn at_boot_an_lcd_with_nothing_saved_is_left_as_it_started() {
        assert_eq!(display_level_at_boot(None, HalDisplayType::Lcd), None);
        assert_eq!(display_level_at_boot(Some(0), HalDisplayType::Lcd), None);
        assert_eq!(display_level_at_boot(Some(70), HalDisplayType::Lcd), Some(70));
    }

    #[test]
    fn an_lcd_is_never_offered_dark() {
        assert!(display_min(HalDisplayType::Lcd) > 0);
        assert_eq!(display_min(HalDisplayType::Epaper), 0);
    }

    #[test]
    fn a_light_is_there_when_any_driver_lit_it() {
        assert_eq!(has_light(true, false), YES);
        assert_eq!(has_light(true, true), YES);
    }

    #[test]
    fn a_light_is_not_there_when_every_driver_said_so() {
        assert_eq!(has_light(false, false), NO);
    }

    #[test]
    fn a_driver_that_failed_says_nothing_about_the_light() {
        assert_eq!(has_light(false, true), UNKNOWN);
    }

    #[test]
    fn a_level_over_100_is_refused_before_it_reaches_a_driver() {
        assert_eq!(light_set_display(101, false), ESP_ERR_INVALID_ARG);
        assert_eq!(light_set_keys(101, false), ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn after_sleep_epaper_with_nothing_saved_stays_dark() {
        assert_eq!(display_level_after_sleep(None, HalDisplayType::Epaper), None);
        assert_eq!(display_level_after_sleep(Some(40), HalDisplayType::Epaper), Some(40));
    }
}
