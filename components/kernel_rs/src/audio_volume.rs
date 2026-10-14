// SPDX-License-Identifier: BSD-3-Clause
//! The system's volume and mute, kept in system.json so they come back after
//! a reboot:
//!
//! ```json
//! "audio": { "volume": 60, "muted": false }
//! ```
//!
//! The audio driver is given 0 while muted and the volume otherwise, at boot
//! and on every change. Muting keeps the volume, so unmuting goes back to it.
//! An app that sets the volume through the `thistle_audio_set_volume` syscall
//! sets this one, unsaved, so the quick panel shows what is playing at.

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};

use crate::hal_registry::registry;

const SECTION: &str = "audio";
const VOLUME_KEY: &str = "volume";
const MUTED_KEY: &str = "muted";

/// What a device that never had a volume set plays at: the ES8311 driver's
/// own default, which LilyGo's speaker handles without distortion.
pub(crate) const DEFAULT_VOLUME: u8 = 50;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

static VOLUME: AtomicU8 = AtomicU8::new(DEFAULT_VOLUME);
static MUTED: AtomicBool = AtomicBool::new(false);

/// What the driver is given.
pub(crate) fn effective_level(volume: u8, muted: bool) -> u8 {
    if muted {
        0
    } else {
        volume.min(100)
    }
}

/// The saved volume, or `None` if there is none or it is not a percent.
pub(crate) fn volume_in(json: Option<&str>) -> Option<u8> {
    let level = crate::system_json::get(json?, SECTION, VOLUME_KEY)?.as_u64()?;
    if level > 100 {
        return None;
    }
    Some(level as u8)
}

/// Whether the device was left muted: not unless saved so.
pub(crate) fn muted_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, MUTED_KEY))
        .and_then(|v| v.as_bool())
        .unwrap_or(false)
}

/// Give the driver the level it should play at now.
fn apply() -> i32 {
    let reg = registry();
    let Some(set_volume) = (unsafe { reg.audio.as_ref() }).and_then(|d| d.set_volume) else {
        return ESP_ERR_NOT_SUPPORTED;
    };
    let level = effective_level(VOLUME.load(Ordering::Relaxed), MUTED.load(Ordering::Relaxed));
    unsafe { set_volume(level) }
}

/// Put back the saved volume and mute. kernel_init() calls this once the
/// drivers have started.
pub(crate) fn restore() {
    let json = std::fs::read_to_string(crate::system_json::path()).ok();
    if let Some(level) = volume_in(json.as_deref()) {
        VOLUME.store(level, Ordering::Relaxed);
    }
    MUTED.store(muted_in(json.as_deref()), Ordering::Relaxed);
    if !registry().audio.is_null() {
        apply();
    }
}

/// An app's volume: the system's, not saved.
pub(crate) fn set_for_app(percent: u8) -> i32 {
    rs_audio_set_volume(percent, false)
}

/// Whether the board can play sound.
#[no_mangle]
pub extern "C" fn rs_audio_available() -> bool {
    unsafe { registry().audio.as_ref() }.is_some_and(|d| d.set_volume.is_some())
}

/// The volume, 0-100, muted or not.
#[no_mangle]
pub extern "C" fn rs_audio_get_volume() -> u8 {
    VOLUME.load(Ordering::Relaxed)
}

/// Set the volume, and save it if `save`. Applies at once; while muted it is
/// kept for when the device is unmuted. The driver's error if it refused;
/// otherwise whether the volume was saved.
#[no_mangle]
pub extern "C" fn rs_audio_set_volume(percent: u8, save: bool) -> i32 {
    if percent > 100 {
        return ESP_ERR_INVALID_ARG;
    }
    VOLUME.store(percent, Ordering::Relaxed);
    let ret = apply();
    if ret != ESP_OK {
        return ret;
    }
    if !save {
        return ESP_OK;
    }
    crate::system_json::save(SECTION, VOLUME_KEY, serde_json::Value::from(percent))
}

#[no_mangle]
pub extern "C" fn rs_audio_is_muted() -> bool {
    MUTED.load(Ordering::Relaxed)
}

/// Mute or unmute, and remember it. The driver's error if it refused;
/// otherwise whether the choice was saved.
#[no_mangle]
pub extern "C" fn rs_audio_set_muted(muted: bool) -> i32 {
    MUTED.store(muted, Ordering::Relaxed);
    let ret = apply();
    if ret != ESP_OK {
        return ret;
    }
    crate::system_json::save(SECTION, MUTED_KEY, serde_json::Value::Bool(muted))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn muted_plays_at_nothing_whatever_the_volume() {
        assert_eq!(effective_level(80, true), 0);
        assert_eq!(effective_level(0, true), 0);
    }

    #[test]
    fn unmuted_plays_at_the_volume() {
        assert_eq!(effective_level(80, false), 80);
        assert_eq!(effective_level(0, false), 0);
        assert_eq!(effective_level(100, false), 100);
    }

    #[test]
    fn a_volume_over_100_plays_at_100() {
        assert_eq!(effective_level(250, false), 100);
    }

    #[test]
    fn reads_the_saved_volume() {
        let json = r#"{ "thistle_os": { "audio": { "volume": 35 } } }"#;
        assert_eq!(volume_in(Some(json)), Some(35));
    }

    #[test]
    fn no_volume_when_none_was_saved() {
        assert_eq!(volume_in(None), None);
        assert_eq!(volume_in(Some(r#"{ "thistle_os": {} }"#)), None);
    }

    #[test]
    fn a_saved_volume_that_is_not_a_percent_is_ignored() {
        assert_eq!(volume_in(Some(r#"{ "thistle_os": { "audio": { "volume": 101 } } }"#)), None);
        assert_eq!(volume_in(Some(r#"{ "thistle_os": { "audio": { "volume": -3 } } }"#)), None);
        assert_eq!(volume_in(Some(r#"{ "thistle_os": { "audio": { "volume": "loud" } } }"#)), None);
    }

    #[test]
    fn not_muted_unless_saved_so() {
        assert!(!muted_in(None));
        assert!(!muted_in(Some(r#"{ "thistle_os": { "audio": { "volume": 20 } } }"#)));
        assert!(muted_in(Some(r#"{ "thistle_os": { "audio": { "muted": true } } }"#)));
        assert!(!muted_in(Some(r#"{ "thistle_os": { "audio": { "muted": 1 } } }"#)));
    }

    #[test]
    fn a_volume_over_100_is_refused_before_it_is_kept() {
        let before = rs_audio_get_volume();
        assert_eq!(rs_audio_set_volume(101, false), ESP_ERR_INVALID_ARG);
        assert_eq!(rs_audio_get_volume(), before);
    }
}
