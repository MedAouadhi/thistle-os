// SPDX-License-Identifier: BSD-3-Clause
//! Which antenna the LoRa radio uses, on a board whose profile declares an RF
//! switch between two, and the choice kept in system.json as
//! `thistle_os.radio.antenna`: `"internal"` or `"external"`.
//!
//! The board profile says where the switch is, and which level selects the
//! antenna inside the case:
//!
//! ```json
//! "radio_antenna_switch": { "expander": 0, "pin": 4, "internal_level": 1 }
//! ```
//!
//! The boot sequence always selects the internal antenna, before any driver
//! loads: it is the one that is always there, and a radio transmitting into an
//! empty connector has its power reflected back into it. The saved choice is
//! applied once the drivers are up and the SD card that may hold system.json
//! is mounted, which is still before anything transmits.
//!
//! Nothing can tell whether an antenna is screwed on — the switch has no sense
//! line — so choosing external is the user's word that one is.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Mutex;

use crate::system_json;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

const SECTION: &str = "radio";
const KEY: &str = "antenna";

// Mirrors radio_antenna_t in components/kernel/include/thistle/radio_antenna.h.
const ANTENNA_INTERNAL: u8 = 0;
const ANTENNA_EXTERNAL: u8 = 1;

#[cfg(not(test))]
static TAG: &[u8] = b"radio_antenna\0";

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
macro_rules! antenna_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}
#[cfg(test)]
macro_rules! antenna_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => { let _ = ($level, $($arg,)*); };
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

/// An RF switch a board profile declares between its radio and two antennas.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct AntennaSwitch {
    /// Index into the profile's `io_expanders` array.
    pub expander: usize,
    pub pin: u8,
    /// The level that routes the radio to the antenna inside the case.
    pub internal_level: bool,
}

/// The switch the boot sequence drove, or `None` on a board with none.
static SWITCH: Mutex<Option<AntennaSwitch>> = Mutex::new(None);
static SELECTED: AtomicU8 = AtomicU8::new(ANTENNA_INTERNAL);

/// Record the switch the board profile declares, or that it declares none.
/// The boot sequence has just thrown it to the internal antenna.
pub(crate) fn declare(switch: Option<AntennaSwitch>) {
    if let Ok(mut declared) = SWITCH.lock() {
        *declared = switch;
    }
    SELECTED.store(ANTENNA_INTERNAL, Ordering::Relaxed);
}

fn declared() -> Option<AntennaSwitch> {
    SWITCH.lock().ok().and_then(|declared| *declared)
}

/// The level that routes `switch` to `antenna`.
fn level_for(switch: &AntennaSwitch, antenna: u8) -> bool {
    if antenna == ANTENNA_EXTERNAL {
        !switch.internal_level
    } else {
        switch.internal_level
    }
}

fn name_of(antenna: u8) -> Option<&'static str> {
    match antenna {
        ANTENNA_INTERNAL => Some("internal"),
        ANTENNA_EXTERNAL => Some("external"),
        _ => None,
    }
}

/// The antenna saved in `json`. Anything but `"external"` is internal, the
/// one that is always fitted.
fn antenna_in(json: &str) -> u8 {
    match system_json::get(json, SECTION, KEY).as_ref().and_then(|v| v.as_str()) {
        Some("external") => ANTENNA_EXTERNAL,
        _ => ANTENNA_INTERNAL,
    }
}

/// Whether this board has an antenna to choose.
#[no_mangle]
pub extern "C" fn radio_antenna_switchable() -> bool {
    declared().is_some()
}

/// The antenna the radio is routed to now.
#[no_mangle]
pub extern "C" fn radio_antenna_get() -> u8 {
    SELECTED.load(Ordering::Relaxed)
}

/// Route the radio to `antenna`, now. Does not save the choice.
///
/// `ESP_ERR_NOT_SUPPORTED` on a board with no switch; otherwise whatever the
/// expander answered, in which case the radio stays where it was.
#[no_mangle]
pub extern "C" fn radio_antenna_select(antenna: u8) -> i32 {
    if name_of(antenna).is_none() {
        return ESP_ERR_INVALID_ARG;
    }
    let Some(switch) = declared() else {
        return ESP_ERR_NOT_SUPPORTED;
    };
    let ret = crate::drv_ioexp_xl9555::thistle_expander_set_level(
        switch.expander as i32,
        switch.pin as i32,
        level_for(&switch, antenna) as i32,
    );
    if ret == ESP_OK {
        SELECTED.store(antenna, Ordering::Relaxed);
    }
    ret
}

/// Save the antenna choice for the next boot.
#[no_mangle]
pub extern "C" fn radio_antenna_save(antenna: u8) -> i32 {
    match name_of(antenna) {
        Some(name) => system_json::save(SECTION, KEY, serde_json::Value::from(name)),
        None => ESP_ERR_INVALID_ARG,
    }
}

/// Route the radio to the antenna saved in system.json. Call once the drivers
/// have started and before anything transmits.
#[no_mangle]
pub extern "C" fn radio_antenna_restore() {
    if declared().is_none() {
        return;
    }
    let Ok(json) = std::fs::read_to_string(system_json::path()) else {
        return;
    };
    if antenna_in(&json) != ANTENNA_EXTERNAL {
        return;
    }
    let ret = radio_antenna_select(ANTENNA_EXTERNAL);
    if ret == ESP_OK {
        antenna_log!(ESP_LOG_INFO, b"radio: external antenna selected\n\0");
    } else {
        antenna_log!(
            ESP_LOG_WARN,
            b"radio: could not select the external antenna (%d); staying internal\n\0",
            ret
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MAX_SWITCH: AntennaSwitch = AntennaSwitch {
        expander: 0,
        pin: 4,
        internal_level: true,
    };

    fn file_with(antenna: &str) -> String {
        format!(r#"{{ "thistle_os": {{ "radio": {{ "antenna": "{antenna}" }} }} }}"#)
    }

    #[test]
    fn test_each_antenna_round_trips_through_its_name() {
        for antenna in [ANTENNA_INTERNAL, ANTENNA_EXTERNAL] {
            assert_eq!(antenna_in(&file_with(name_of(antenna).unwrap())), antenna);
        }
    }

    #[test]
    fn test_no_saved_choice_reads_as_internal() {
        // The shipped file has a radio section with no antenna in it.
        let shipped = r#"{ "thistle_os": { "radio": { "frequency_mhz": 915.0 } } }"#;
        assert_eq!(antenna_in(shipped), ANTENNA_INTERNAL);
        assert_eq!(antenna_in(r#"{ "thistle_os": {} }"#), ANTENNA_INTERNAL);
        assert_eq!(antenna_in("{ not json"), ANTENNA_INTERNAL);
    }

    #[test]
    fn test_anything_but_external_reads_as_internal() {
        // A choice that routed to an empty connector by mistake would transmit
        // into it, so only the exact word selects the outside one.
        assert_eq!(antenna_in(&file_with("External")), ANTENNA_INTERNAL);
        assert_eq!(antenna_in(&file_with("sma")), ANTENNA_INTERNAL);
        let numeric = r#"{ "thistle_os": { "radio": { "antenna": 1 } } }"#;
        assert_eq!(antenna_in(numeric), ANTENNA_INTERNAL);
    }

    #[test]
    fn test_the_level_follows_the_profile_not_a_convention() {
        assert!(level_for(&MAX_SWITCH, ANTENNA_INTERNAL));
        assert!(!level_for(&MAX_SWITCH, ANTENNA_EXTERNAL));

        let inverted = AntennaSwitch {
            internal_level: false,
            ..MAX_SWITCH
        };
        assert!(!level_for(&inverted, ANTENNA_INTERNAL));
        assert!(level_for(&inverted, ANTENNA_EXTERNAL));
    }

    #[test]
    fn test_a_board_with_no_switch_offers_no_choice() {
        declare(None);
        assert!(!radio_antenna_switchable());
        assert_eq!(radio_antenna_select(ANTENNA_EXTERNAL), ESP_ERR_NOT_SUPPORTED);
        assert_eq!(radio_antenna_get(), ANTENNA_INTERNAL);
    }

    #[test]
    fn test_an_unknown_antenna_is_refused() {
        declare(Some(MAX_SWITCH));
        assert_eq!(radio_antenna_select(2), ESP_ERR_INVALID_ARG);
        assert_eq!(radio_antenna_save(2), ESP_ERR_INVALID_ARG);
        declare(None);
    }

    #[test]
    fn test_a_switch_the_expander_cannot_reach_stays_where_it_was() {
        // A declared switch on an expander that is not attached: the write
        // fails, and the selection must not claim it moved.
        crate::drv_ioexp_xl9555::detach_all();
        declare(Some(MAX_SWITCH));
        assert_ne!(radio_antenna_select(ANTENNA_EXTERNAL), ESP_OK);
        assert_eq!(radio_antenna_get(), ANTENNA_INTERNAL);
        declare(None);
    }

    #[test]
    fn test_declaring_the_switch_starts_from_internal() {
        // The boot sequence has just driven it there.
        declare(Some(MAX_SWITCH));
        assert!(radio_antenna_switchable());
        assert_eq!(radio_antenna_get(), ANTENNA_INTERNAL);
        declare(None);
    }
}
