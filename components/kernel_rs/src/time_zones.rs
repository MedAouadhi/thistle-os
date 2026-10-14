// SPDX-License-Identifier: BSD-3-Clause
//! The time zones Settings offers, each as its IANA name and the POSIX TZ
//! rule the C library applies.
//!
//! The C library on the device (newlib) and in the simulators (glibc) takes
//! a zone only as a POSIX rule, such as "CET-1CEST,M3.5.0,M10.5.0/3": it has
//! no zoneinfo database. The rules here are the ones tzdata 2025b ends each
//! zone's file with, which say how that zone keeps time from now on. A
//! country that changes its rules needs its entry changed here.
//!
//! POSIX counts offsets west of Greenwich as positive, so "CET-1" is UTC+1.
//! A zone with no abbreviation of its own has its offset as one, quoted in
//! angle brackets: "<+04>-4".

use std::ffi::CStr;

pub(crate) struct Zone {
    /// The IANA name, "Region/City", which system.json keeps.
    pub name: &'static CStr,
    pub posix: &'static str,
}

const UTC: &str = "UTC0";
const GMT: &str = "GMT0";
const UK: &str = "GMT0BST,M3.5.0/1,M10.5.0";
// Ireland's own rule is the same clock written with winter as the
// exception, "IST-1GMT0,M10.5.0,M3.5.0/1"; this is the plainer form of it.
const IRELAND: &str = "GMT0IST,M3.5.0/1,M10.5.0";
const WESTERN_EU: &str = "WET0WEST,M3.5.0/1,M10.5.0";
const CENTRAL_EU: &str = "CET-1CEST,M3.5.0,M10.5.0/3";
const EASTERN_EU: &str = "EET-2EEST,M3.5.0/3,M10.5.0/4";
const US_EASTERN: &str = "EST5EDT,M3.2.0,M11.1.0";
const US_CENTRAL: &str = "CST6CDT,M3.2.0,M11.1.0";
const US_MOUNTAIN: &str = "MST7MDT,M3.2.0,M11.1.0";
const US_PACIFIC: &str = "PST8PDT,M3.2.0,M11.1.0";
const AU_EASTERN: &str = "AEST-10AEDT,M10.1.0,M4.1.0/3";

pub(crate) static ZONES: &[Zone] = &[
    Zone { name: c"UTC", posix: UTC },
    Zone { name: c"Africa/Accra", posix: GMT },
    Zone { name: c"Africa/Algiers", posix: "CET-1" },
    Zone { name: c"Africa/Cairo", posix: "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    Zone { name: c"Africa/Casablanca", posix: "<+01>-1" },
    Zone { name: c"Africa/Johannesburg", posix: "SAST-2" },
    Zone { name: c"Africa/Lagos", posix: "WAT-1" },
    Zone { name: c"Africa/Nairobi", posix: "EAT-3" },
    Zone { name: c"Africa/Tunis", posix: "CET-1" },
    Zone { name: c"America/Anchorage", posix: "AKST9AKDT,M3.2.0,M11.1.0" },
    Zone { name: c"America/Argentina/Buenos_Aires", posix: "<-03>3" },
    Zone { name: c"America/Bogota", posix: "<-05>5" },
    Zone { name: c"America/Caracas", posix: "<-04>4" },
    Zone { name: c"America/Chicago", posix: US_CENTRAL },
    Zone { name: c"America/Denver", posix: US_MOUNTAIN },
    Zone { name: c"America/Edmonton", posix: US_MOUNTAIN },
    Zone { name: c"America/Guatemala", posix: "CST6" },
    Zone { name: c"America/Halifax", posix: "AST4ADT,M3.2.0,M11.1.0" },
    Zone { name: c"America/Havana", posix: "CST5CDT,M3.2.0/0,M11.1.0/1" },
    Zone { name: c"America/La_Paz", posix: "<-04>4" },
    Zone { name: c"America/Lima", posix: "<-05>5" },
    Zone { name: c"America/Los_Angeles", posix: US_PACIFIC },
    Zone { name: c"America/Mexico_City", posix: "CST6" },
    Zone { name: c"America/Montevideo", posix: "<-03>3" },
    Zone { name: c"America/New_York", posix: US_EASTERN },
    Zone { name: c"America/Panama", posix: "EST5" },
    Zone { name: c"America/Phoenix", posix: "MST7" },
    Zone { name: c"America/Puerto_Rico", posix: "AST4" },
    Zone { name: c"America/Regina", posix: "CST6" },
    Zone { name: c"America/Santiago", posix: "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    Zone { name: c"America/Sao_Paulo", posix: "<-03>3" },
    Zone { name: c"America/St_Johns", posix: "NST3:30NDT,M3.2.0,M11.1.0" },
    Zone { name: c"America/Toronto", posix: US_EASTERN },
    Zone { name: c"America/Vancouver", posix: US_PACIFIC },
    Zone { name: c"America/Winnipeg", posix: US_CENTRAL },
    Zone { name: c"Asia/Almaty", posix: "<+05>-5" },
    Zone { name: c"Asia/Amman", posix: "<+03>-3" },
    Zone { name: c"Asia/Baghdad", posix: "<+03>-3" },
    Zone { name: c"Asia/Baku", posix: "<+04>-4" },
    Zone { name: c"Asia/Bangkok", posix: "<+07>-7" },
    Zone { name: c"Asia/Beirut", posix: "EET-2EEST,M3.5.0/0,M10.5.0/0" },
    Zone { name: c"Asia/Colombo", posix: "<+0530>-5:30" },
    Zone { name: c"Asia/Dhaka", posix: "<+06>-6" },
    Zone { name: c"Asia/Dubai", posix: "<+04>-4" },
    Zone { name: c"Asia/Ho_Chi_Minh", posix: "<+07>-7" },
    Zone { name: c"Asia/Hong_Kong", posix: "HKT-8" },
    Zone { name: c"Asia/Jakarta", posix: "WIB-7" },
    Zone { name: c"Asia/Jerusalem", posix: "IST-2IDT,M3.4.4/26,M10.5.0" },
    Zone { name: c"Asia/Kabul", posix: "<+0430>-4:30" },
    Zone { name: c"Asia/Karachi", posix: "PKT-5" },
    Zone { name: c"Asia/Kathmandu", posix: "<+0545>-5:45" },
    Zone { name: c"Asia/Kolkata", posix: "IST-5:30" },
    Zone { name: c"Asia/Kuala_Lumpur", posix: "<+08>-8" },
    Zone { name: c"Asia/Manila", posix: "PST-8" },
    Zone { name: c"Asia/Novosibirsk", posix: "<+07>-7" },
    Zone { name: c"Asia/Riyadh", posix: "<+03>-3" },
    Zone { name: c"Asia/Seoul", posix: "KST-9" },
    Zone { name: c"Asia/Shanghai", posix: "CST-8" },
    Zone { name: c"Asia/Singapore", posix: "<+08>-8" },
    Zone { name: c"Asia/Taipei", posix: "CST-8" },
    Zone { name: c"Asia/Tashkent", posix: "<+05>-5" },
    Zone { name: c"Asia/Tbilisi", posix: "<+04>-4" },
    Zone { name: c"Asia/Tehran", posix: "<+0330>-3:30" },
    Zone { name: c"Asia/Tokyo", posix: "JST-9" },
    Zone { name: c"Asia/Vladivostok", posix: "<+10>-10" },
    Zone { name: c"Asia/Yangon", posix: "<+0630>-6:30" },
    Zone { name: c"Asia/Yekaterinburg", posix: "<+05>-5" },
    Zone { name: c"Asia/Yerevan", posix: "<+04>-4" },
    Zone { name: c"Atlantic/Azores", posix: "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
    Zone { name: c"Atlantic/Canary", posix: WESTERN_EU },
    Zone { name: c"Atlantic/Cape_Verde", posix: "<-01>1" },
    Zone { name: c"Atlantic/Reykjavik", posix: GMT },
    Zone { name: c"Australia/Adelaide", posix: "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    Zone { name: c"Australia/Brisbane", posix: "AEST-10" },
    Zone { name: c"Australia/Darwin", posix: "ACST-9:30" },
    Zone { name: c"Australia/Hobart", posix: AU_EASTERN },
    Zone { name: c"Australia/Melbourne", posix: AU_EASTERN },
    Zone { name: c"Australia/Perth", posix: "AWST-8" },
    Zone { name: c"Australia/Sydney", posix: AU_EASTERN },
    Zone { name: c"Europe/Amsterdam", posix: CENTRAL_EU },
    Zone { name: c"Europe/Athens", posix: EASTERN_EU },
    Zone { name: c"Europe/Belgrade", posix: CENTRAL_EU },
    Zone { name: c"Europe/Berlin", posix: CENTRAL_EU },
    Zone { name: c"Europe/Brussels", posix: CENTRAL_EU },
    Zone { name: c"Europe/Bucharest", posix: EASTERN_EU },
    Zone { name: c"Europe/Budapest", posix: CENTRAL_EU },
    Zone { name: c"Europe/Copenhagen", posix: CENTRAL_EU },
    Zone { name: c"Europe/Dublin", posix: IRELAND },
    Zone { name: c"Europe/Helsinki", posix: EASTERN_EU },
    Zone { name: c"Europe/Istanbul", posix: "<+03>-3" },
    Zone { name: c"Europe/Kaliningrad", posix: "EET-2" },
    Zone { name: c"Europe/Kyiv", posix: EASTERN_EU },
    Zone { name: c"Europe/Lisbon", posix: WESTERN_EU },
    Zone { name: c"Europe/London", posix: UK },
    Zone { name: c"Europe/Madrid", posix: CENTRAL_EU },
    Zone { name: c"Europe/Minsk", posix: "<+03>-3" },
    Zone { name: c"Europe/Moscow", posix: "MSK-3" },
    Zone { name: c"Europe/Oslo", posix: CENTRAL_EU },
    Zone { name: c"Europe/Paris", posix: CENTRAL_EU },
    Zone { name: c"Europe/Prague", posix: CENTRAL_EU },
    Zone { name: c"Europe/Riga", posix: EASTERN_EU },
    Zone { name: c"Europe/Rome", posix: CENTRAL_EU },
    Zone { name: c"Europe/Samara", posix: "<+04>-4" },
    Zone { name: c"Europe/Sofia", posix: EASTERN_EU },
    Zone { name: c"Europe/Stockholm", posix: CENTRAL_EU },
    Zone { name: c"Europe/Tallinn", posix: EASTERN_EU },
    Zone { name: c"Europe/Vienna", posix: CENTRAL_EU },
    Zone { name: c"Europe/Vilnius", posix: EASTERN_EU },
    Zone { name: c"Europe/Warsaw", posix: CENTRAL_EU },
    Zone { name: c"Europe/Zurich", posix: CENTRAL_EU },
    Zone { name: c"Indian/Maldives", posix: "<+05>-5" },
    Zone { name: c"Indian/Mauritius", posix: "<+04>-4" },
    Zone { name: c"Pacific/Auckland", posix: "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    Zone { name: c"Pacific/Chatham", posix: "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45" },
    Zone { name: c"Pacific/Fiji", posix: "<+12>-12" },
    Zone { name: c"Pacific/Guam", posix: "ChST-10" },
    Zone { name: c"Pacific/Honolulu", posix: "HST10" },
    Zone { name: c"Pacific/Pago_Pago", posix: "SST11" },
    Zone { name: c"Pacific/Port_Moresby", posix: "<+10>-10" },
    Zone { name: c"Pacific/Tongatapu", posix: "<+13>-13" },
];

/// The POSIX rule of the zone named `name`, if it is one of these.
pub(crate) fn posix_for(name: &str) -> Option<&'static str> {
    ZONES
        .iter()
        .find(|z| z.name.to_bytes() == name.as_bytes())
        .map(|z| z.posix)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::time_manager::libc_time;

    /// Each zone's offset from UTC, in minutes, on 15 January and 15 July
    /// 2026 at 12:00 UTC: one date in each half of the year, so a zone with
    /// summer time shows both of its offsets whichever hemisphere it is in.
    const EXPECTED: &[(&str, i32, i32)] = &[
        ("UTC", 0, 0),
        ("Africa/Accra", 0, 0),
        ("Africa/Algiers", 60, 60),
        ("Africa/Cairo", 120, 180),
        ("Africa/Casablanca", 60, 60),
        ("Africa/Johannesburg", 120, 120),
        ("Africa/Lagos", 60, 60),
        ("Africa/Nairobi", 180, 180),
        ("Africa/Tunis", 60, 60),
        ("America/Anchorage", -540, -480),
        ("America/Argentina/Buenos_Aires", -180, -180),
        ("America/Bogota", -300, -300),
        ("America/Caracas", -240, -240),
        ("America/Chicago", -360, -300),
        ("America/Denver", -420, -360),
        ("America/Edmonton", -420, -360),
        ("America/Guatemala", -360, -360),
        ("America/Halifax", -240, -180),
        ("America/Havana", -300, -240),
        ("America/La_Paz", -240, -240),
        ("America/Lima", -300, -300),
        ("America/Los_Angeles", -480, -420),
        ("America/Mexico_City", -360, -360),
        ("America/Montevideo", -180, -180),
        ("America/New_York", -300, -240),
        ("America/Panama", -300, -300),
        ("America/Phoenix", -420, -420),
        ("America/Puerto_Rico", -240, -240),
        ("America/Regina", -360, -360),
        ("America/Santiago", -180, -240),
        ("America/Sao_Paulo", -180, -180),
        ("America/St_Johns", -210, -150),
        ("America/Toronto", -300, -240),
        ("America/Vancouver", -480, -420),
        ("America/Winnipeg", -360, -300),
        ("Asia/Almaty", 300, 300),
        ("Asia/Amman", 180, 180),
        ("Asia/Baghdad", 180, 180),
        ("Asia/Baku", 240, 240),
        ("Asia/Bangkok", 420, 420),
        ("Asia/Beirut", 120, 180),
        ("Asia/Colombo", 330, 330),
        ("Asia/Dhaka", 360, 360),
        ("Asia/Dubai", 240, 240),
        ("Asia/Ho_Chi_Minh", 420, 420),
        ("Asia/Hong_Kong", 480, 480),
        ("Asia/Jakarta", 420, 420),
        ("Asia/Jerusalem", 120, 180),
        ("Asia/Kabul", 270, 270),
        ("Asia/Karachi", 300, 300),
        ("Asia/Kathmandu", 345, 345),
        ("Asia/Kolkata", 330, 330),
        ("Asia/Kuala_Lumpur", 480, 480),
        ("Asia/Manila", 480, 480),
        ("Asia/Novosibirsk", 420, 420),
        ("Asia/Riyadh", 180, 180),
        ("Asia/Seoul", 540, 540),
        ("Asia/Shanghai", 480, 480),
        ("Asia/Singapore", 480, 480),
        ("Asia/Taipei", 480, 480),
        ("Asia/Tashkent", 300, 300),
        ("Asia/Tbilisi", 240, 240),
        ("Asia/Tehran", 210, 210),
        ("Asia/Tokyo", 540, 540),
        ("Asia/Vladivostok", 600, 600),
        ("Asia/Yangon", 390, 390),
        ("Asia/Yekaterinburg", 300, 300),
        ("Asia/Yerevan", 240, 240),
        ("Atlantic/Azores", -60, 0),
        ("Atlantic/Canary", 0, 60),
        ("Atlantic/Cape_Verde", -60, -60),
        ("Atlantic/Reykjavik", 0, 0),
        ("Australia/Adelaide", 630, 570),
        ("Australia/Brisbane", 600, 600),
        ("Australia/Darwin", 570, 570),
        ("Australia/Hobart", 660, 600),
        ("Australia/Melbourne", 660, 600),
        ("Australia/Perth", 480, 480),
        ("Australia/Sydney", 660, 600),
        ("Europe/Amsterdam", 60, 120),
        ("Europe/Athens", 120, 180),
        ("Europe/Belgrade", 60, 120),
        ("Europe/Berlin", 60, 120),
        ("Europe/Brussels", 60, 120),
        ("Europe/Bucharest", 120, 180),
        ("Europe/Budapest", 60, 120),
        ("Europe/Copenhagen", 60, 120),
        ("Europe/Dublin", 0, 60),
        ("Europe/Helsinki", 120, 180),
        ("Europe/Istanbul", 180, 180),
        ("Europe/Kaliningrad", 120, 120),
        ("Europe/Kyiv", 120, 180),
        ("Europe/Lisbon", 0, 60),
        ("Europe/London", 0, 60),
        ("Europe/Madrid", 60, 120),
        ("Europe/Minsk", 180, 180),
        ("Europe/Moscow", 180, 180),
        ("Europe/Oslo", 60, 120),
        ("Europe/Paris", 60, 120),
        ("Europe/Prague", 60, 120),
        ("Europe/Riga", 120, 180),
        ("Europe/Rome", 60, 120),
        ("Europe/Samara", 240, 240),
        ("Europe/Sofia", 120, 180),
        ("Europe/Stockholm", 60, 120),
        ("Europe/Tallinn", 120, 180),
        ("Europe/Vienna", 60, 120),
        ("Europe/Vilnius", 120, 180),
        ("Europe/Warsaw", 60, 120),
        ("Europe/Zurich", 60, 120),
        ("Indian/Maldives", 300, 300),
        ("Indian/Mauritius", 240, 240),
        ("Pacific/Auckland", 780, 720),
        ("Pacific/Chatham", 825, 765),
        ("Pacific/Fiji", 720, 720),
        ("Pacific/Guam", 600, 600),
        ("Pacific/Honolulu", -600, -600),
        ("Pacific/Pago_Pago", -660, -660),
        ("Pacific/Port_Moresby", 600, 600),
        ("Pacific/Tongatapu", 780, 780),
    ];

    const JAN_15: i64 = 1_768_478_400; // 2026-01-15T12:00:00Z
    const JUL_15: i64 = 1_784_116_800; // 2026-07-15T12:00:00Z

    #[test]
    fn every_zone_keeps_the_time_tzdata_says_in_winter_and_summer() {
        assert_eq!(ZONES.len(), EXPECTED.len(), "a zone without an expected offset, or one too many");
        for (zone, &(name, jan, jul)) in ZONES.iter().zip(EXPECTED) {
            assert_eq!(zone.name.to_str().unwrap(), name, "EXPECTED is out of step with ZONES");
            libc_time::apply_tz(zone.posix);
            assert_eq!(libc_time::offset_minutes(JAN_15), jan, "{name} in January");
            assert_eq!(libc_time::offset_minutes(JUL_15), jul, "{name} in July");
        }
        libc_time::apply_tz(UTC);
    }

    #[test]
    fn summer_time_starts_and_ends_on_the_hour_the_rule_says() {
        // Europe: the last Sunday of March at 01:00 UTC, 29 March 2026.
        libc_time::apply_tz(CENTRAL_EU);
        assert_eq!(libc_time::offset_minutes(1_774_745_999), 60);
        assert_eq!(libc_time::offset_minutes(1_774_746_000), 120);
        // The US: the second Sunday of March at 02:00 local, 8 March 2026.
        libc_time::apply_tz(US_EASTERN);
        assert_eq!(libc_time::offset_minutes(1_772_953_199), -300);
        assert_eq!(libc_time::offset_minutes(1_772_953_200), -240);
        libc_time::apply_tz(UTC);
    }

    #[test]
    fn names_are_unique_and_sorted_within_their_region() {
        let names: Vec<&str> = ZONES.iter().map(|z| z.name.to_str().unwrap()).collect();
        assert_eq!(names[0], "UTC");
        for pair in names[1..].windows(2) {
            assert!(pair[0] < pair[1], "{} before {}", pair[0], pair[1]);
        }
    }

    #[test]
    fn a_zone_is_found_by_its_name_only() {
        assert_eq!(posix_for("Europe/Oslo"), Some(CENTRAL_EU));
        assert_eq!(posix_for("Europe/oslo"), None);
        assert_eq!(posix_for("Oslo"), None);
        assert_eq!(posix_for(""), None);
    }
}
