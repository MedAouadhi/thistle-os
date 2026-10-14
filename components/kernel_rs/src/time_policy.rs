// SPDX-License-Identifier: BSD-3-Clause
//! What the clock believes, decided without touching it.
//!
//! Several sources offer the time, each as good as it is:
//!
//! | Source  | Comes from                          | Good to       |
//! |---------|-------------------------------------|---------------|
//! | NTP     | a time server, over WiFi or LTE     | milliseconds  |
//! | GNSS    | the GPS receiver's fix ($GNRMC)     | about 1 s     |
//! | Network | the mobile network (NITZ), via the modem's clock | about 1 s |
//! | RTC     | a battery-backed clock chip         | seconds a day |
//!
//! The best of them wins. A worse one is taken only when the clock has not
//! been set, or when nothing better has confirmed it for an hour, and then
//! only if it disagrees by more than it can itself be trusted to: a GNSS
//! time a second behind an NTP time is not a reason to move the clock.
//!
//! The clock moves by stepping (settimeofday) or by slewing (adjtime, which
//! ESP-IDF runs 1/64 fast or slow until the difference is gone). Anything
//! within SLEW_MAX is slewed, so the time never jumps back for a small
//! correction; anything more is stepped, since a slew of minutes would take
//! hours.
//!
//! With automatic time off, only the user sets the clock. A clock chip still
//! sets it after a power loss, since it then holds what the user set.
//!
//! Times are microseconds since 1970-01-01 UTC throughout.

/// 2026-01-01T00:00:00Z. Nothing before it is a real time: it is a clock
/// that was never set, or a modem's clock counting from its own epoch. Move
/// it forward now and then; a device whose clock is set before it says
/// "--:--".
pub(crate) const FLOOR_S: i64 = 1_767_225_600;
/// 2100-01-01T00:00:00Z, past which the PCF8563's century bit cannot count.
pub(crate) const CEILING_S: i64 = 4_102_444_800;

const US: i64 = 1_000_000;

/// How long a source's word stands against a worse one.
pub(crate) const FRESH_US: i64 = 3600 * US;
/// The largest correction slewed rather than stepped: 10 s takes ESP-IDF's
/// adjtime about 11 minutes.
pub(crate) const SLEW_MAX_US: i64 = 10 * US;

/// What set the clock. Mirrors time_source_t in thistle/time_manager.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Source {
    None = 0,
    /// Set before the last restart or deep sleep, by something not recorded.
    Kept = 1,
    Rtc = 2,
    Network = 3,
    Gnss = 4,
    Ntp = 5,
    Manual = 6,
}

impl Source {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        Some(match value {
            0 => Source::None,
            1 => Source::Kept,
            2 => Source::Rtc,
            3 => Source::Network,
            4 => Source::Gnss,
            5 => Source::Ntp,
            6 => Source::Manual,
            _ => return None,
        })
    }

    /// Which wins. The user's own setting ranks nowhere: it stands only
    /// while automatic time is off, and once it is back on, the first
    /// source heard should replace it.
    fn rank(self) -> u8 {
        match self {
            Source::None | Source::Kept | Source::Manual => 0,
            Source::Rtc => 1,
            Source::Network => 2,
            Source::Gnss => 3,
            Source::Ntp => 4,
        }
    }

    /// How far off the source itself may be. NMEA gives whole seconds, sent
    /// up to a second after the second they name; NITZ and a clock chip
    /// give whole seconds too.
    fn tolerance_us(self) -> i64 {
        match self {
            Source::Ntp | Source::Manual => 0,
            _ => 2 * US,
        }
    }

    /// As Settings says it: "Set by NTP".
    pub(crate) fn label(self) -> &'static str {
        match self {
            Source::None => "not set",
            Source::Kept => "kept",
            Source::Rtc => "the clock chip",
            Source::Network => "the mobile network",
            Source::Gnss => "GPS",
            Source::Ntp => "NTP",
            Source::Manual => "hand",
        }
    }
}

/// The last time a source set or confirmed the clock.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Sync {
    pub source: Source,
    pub at_us: i64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Verdict {
    /// Set the clock to the offered time.
    Step,
    /// Run the clock fast or slow until it has caught up.
    Slew,
    /// The clock is right as it is; note who said so.
    Confirm,
    Ignore,
}

/// Whether `t_us` could be the time now.
pub(crate) fn plausible(t_us: i64) -> bool {
    (FLOOR_S * US..CEILING_S * US).contains(&t_us)
}

/// What to do with `offered_us` from `source`, the clock reading `now_us`
/// and last set or confirmed as `last` says.
pub(crate) fn judge(
    last: Option<Sync>,
    automatic: bool,
    source: Source,
    offered_us: i64,
    now_us: i64,
) -> Verdict {
    if !plausible(offered_us) || source == Source::None || source == Source::Kept {
        return Verdict::Ignore;
    }
    let known = plausible(now_us);
    let correction = || {
        let diff = (offered_us - now_us).abs();
        if diff <= source.tolerance_us() {
            Verdict::Confirm
        } else if diff <= SLEW_MAX_US && known {
            Verdict::Slew
        } else {
            Verdict::Step
        }
    };

    if source == Source::Manual {
        return Verdict::Step;
    }
    if !automatic {
        return if source == Source::Rtc && !known {
            Verdict::Step
        } else {
            Verdict::Ignore
        };
    }
    if !known {
        return Verdict::Step;
    }

    let outranked = match last {
        Some(sync) => {
            let age = now_us - sync.at_us;
            let fresh = (0..FRESH_US).contains(&age);
            fresh && source.rank() < sync.source.rank()
        }
        None => false,
    };
    if outranked {
        Verdict::Ignore
    } else {
        correction()
    }
}

// ---------------------------------------------------------------------------
// The calendar
// ---------------------------------------------------------------------------

/// Days from 1970-01-01 to the given date of the proleptic Gregorian
/// calendar (Howard Hinnant's days_from_civil).
pub(crate) fn days_from_civil(year: i64, month: u32, day: u32) -> i64 {
    let y = if month <= 2 { year - 1 } else { year };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let m = month as i64;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + day as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// The date `days` after 1970-01-01: (year, month 1-12, day 1-31).
pub(crate) fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let year = yoe + era * 400 + if month <= 2 { 1 } else { 0 };
    (year, month, day)
}

fn days_in_month(year: i64, month: u32) -> u32 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if (year % 4 == 0 && year % 100 != 0) || year % 400 == 0 => 29,
        2 => 28,
        _ => 0,
    }
}

/// A date and time of day, in no particular zone.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Civil {
    pub year: i64,
    pub month: u32,
    pub day: u32,
    pub hour: u32,
    pub minute: u32,
    pub second: u32,
}

impl Civil {
    /// None unless every field is in range for its month.
    pub(crate) fn new(year: i64, month: u32, day: u32, hour: u32, minute: u32, second: u32) -> Option<Self> {
        let ok = (1..=12).contains(&month)
            && day >= 1
            && day <= days_in_month(year, month)
            && hour < 24
            && minute < 60
            && second < 60;
        ok.then_some(Civil { year, month, day, hour, minute, second })
    }

    /// Seconds since 1970, taking this to be UTC.
    pub(crate) fn as_utc_seconds(&self) -> i64 {
        days_from_civil(self.year, self.month, self.day) * 86_400
            + (self.hour * 3600 + self.minute * 60 + self.second) as i64
    }

    pub(crate) fn from_utc_seconds(t: i64) -> Self {
        let (year, month, day) = civil_from_days(t.div_euclid(86_400));
        let secs = t.rem_euclid(86_400) as u32;
        Civil { year, month, day, hour: secs / 3600, minute: secs % 3600 / 60, second: secs % 60 }
    }

    /// 0 for Sunday, as the PCF8563 and struct tm count.
    pub(crate) fn weekday(&self) -> u32 {
        (days_from_civil(self.year, self.month, self.day) + 4).rem_euclid(7) as u32
    }
}

/// A date and time as someone types it into Settings: "2026-10-13 14:05",
/// with seconds if they like, and a 'T' instead of the space if they like.
pub(crate) fn parse_typed(text: &str) -> Option<Civil> {
    let text = text.trim();
    let (date, time) = text.split_once([' ', 'T'])?;
    let mut d = date.split('-');
    let year: i64 = d.next()?.parse().ok()?;
    let month: u32 = d.next()?.parse().ok()?;
    let day: u32 = d.next()?.parse().ok()?;
    if d.next().is_some() || !(1000..=9999).contains(&year) {
        return None;
    }
    let mut t = time.trim().split(':');
    let hour: u32 = t.next()?.parse().ok()?;
    let minute: u32 = t.next()?.parse().ok()?;
    let second: u32 = match t.next() {
        Some(s) => s.parse().ok()?,
        None => 0,
    };
    if t.next().is_some() {
        return None;
    }
    Civil::new(year, month, day, hour, minute, second)
}

// ---------------------------------------------------------------------------
// Zones
// ---------------------------------------------------------------------------

/// The POSIX rule for a fixed offset east of UTC, in minutes: what the
/// mobile network gives. "<+02>-2", "<+0530>-5:30", "<-0330>3:30".
pub(crate) fn posix_for_offset(minutes: i32) -> String {
    if minutes == 0 {
        return "UTC0".to_string();
    }
    let sign = if minutes > 0 { '+' } else { '-' };
    let posix_sign = if minutes > 0 { "-" } else { "" };
    let (h, m) = (minutes.abs() / 60, minutes.abs() % 60);
    if m == 0 {
        format!("<{sign}{h:02}>{posix_sign}{h}")
    } else {
        format!("<{sign}{h:02}{m:02}>{posix_sign}{h}:{m:02}")
    }
}

/// "UTC+02:00", "UTC-03:30", "UTC".
pub(crate) fn offset_label(minutes: i32) -> String {
    if minutes == 0 {
        return "UTC".to_string();
    }
    let sign = if minutes > 0 { '+' } else { '-' };
    format!("UTC{sign}{:02}:{:02}", minutes.abs() / 60, minutes.abs() % 60)
}

/// A UTC offset the network could mean: whole quarters of an hour, within
/// the -12:00 to +14:00 that zones span.
pub(crate) fn offset_is_valid(minutes: i32) -> bool {
    minutes % 15 == 0 && (-12 * 60..=14 * 60).contains(&minutes)
}

// ---------------------------------------------------------------------------
// Saying it
// ---------------------------------------------------------------------------

/// "14:05", or "2:05 PM" on a 12-hour clock.
pub(crate) fn clock_face(hour: u32, minute: u32, format_24h: bool) -> String {
    if format_24h {
        return format!("{hour:02}:{minute:02}");
    }
    let suffix = if hour < 12 { "AM" } else { "PM" };
    let h = match hour % 12 {
        0 => 12,
        h => h,
    };
    format!("{h}:{minute:02} {suffix}")
}

/// How long ago, as Settings says it.
pub(crate) fn ago(seconds: i64) -> String {
    match seconds {
        s if s < 60 => "just now".to_string(),
        s if s < 3600 => format!("{} min ago", s / 60),
        s if s < 2 * 86_400 => format!("{} h ago", s / 3600),
        s => format!("{} days ago", s / 86_400),
    }
}

/// The line under the time in Settings: what set the clock, and when.
pub(crate) fn describe(known: bool, last: Option<Sync>, now_us: i64) -> String {
    if !known {
        return "Not set yet".to_string();
    }
    match last {
        Some(Sync { source: Source::Manual, .. }) => "Set by hand".to_string(),
        Some(sync) if sync.source != Source::Kept && sync.at_us <= now_us => {
            format!("Set by {}, {}", sync.source.label(), ago((now_us - sync.at_us) / US))
        }
        _ => "Kept since before the last restart".to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: i64 = 1_791_892_800 * US; // 2026-10-13T12:00:00Z
    const UNSET: i64 = 12 * US; // twelve seconds after a power-on

    fn synced(source: Source, ago_s: i64) -> Option<Sync> {
        Some(Sync { source, at_us: NOW - ago_s * US })
    }

    #[test]
    fn any_source_sets_a_clock_that_was_never_set() {
        for source in [Source::Rtc, Source::Network, Source::Gnss, Source::Ntp] {
            assert_eq!(judge(None, true, source, NOW, UNSET), Verdict::Step, "{source:?}");
        }
    }

    #[test]
    fn a_time_before_2026_or_after_2099_is_no_time_at_all() {
        let modem_epoch = 315_964_800 * US; // the 1980 a modem's clock starts from
        assert_eq!(judge(None, true, Source::Network, modem_epoch, UNSET), Verdict::Ignore);
        assert_eq!(judge(None, true, Source::Gnss, CEILING_S * US, UNSET), Verdict::Ignore);
        assert!(plausible(FLOOR_S * US));
        assert!(!plausible(FLOOR_S * US - 1));
    }

    #[test]
    fn a_better_source_corrects_by_slewing_small_differences_and_stepping_large_ones() {
        let last = synced(Source::Network, 60);
        assert_eq!(judge(last, true, Source::Ntp, NOW + 3 * US, NOW), Verdict::Slew);
        assert_eq!(judge(last, true, Source::Ntp, NOW - 10 * US, NOW), Verdict::Slew);
        assert_eq!(judge(last, true, Source::Ntp, NOW - 11 * US, NOW), Verdict::Step);
        assert_eq!(judge(last, true, Source::Ntp, NOW + 1800 * US, NOW), Verdict::Step);
    }

    #[test]
    fn ntp_corrects_even_milliseconds_since_nothing_is_better() {
        let last = synced(Source::Ntp, 3600);
        assert_eq!(judge(last, true, Source::Ntp, NOW + 4_000, NOW), Verdict::Slew);
        assert_eq!(judge(last, true, Source::Ntp, NOW, NOW), Verdict::Confirm);
    }

    #[test]
    fn a_source_within_its_own_precision_only_confirms() {
        let last = synced(Source::Gnss, 600);
        assert_eq!(judge(last, true, Source::Gnss, NOW - 2 * US, NOW), Verdict::Confirm);
        assert_eq!(judge(last, true, Source::Gnss, NOW - 2 * US - 1, NOW), Verdict::Slew);
    }

    #[test]
    fn a_worse_source_waits_an_hour_after_a_better_one() {
        let ntp_recently = synced(Source::Ntp, 59 * 60);
        assert_eq!(judge(ntp_recently, true, Source::Gnss, NOW + 5 * US, NOW), Verdict::Ignore);
        assert_eq!(judge(ntp_recently, true, Source::Network, NOW + 600 * US, NOW), Verdict::Ignore);
        let ntp_long_ago = synced(Source::Ntp, 61 * 60);
        assert_eq!(judge(ntp_long_ago, true, Source::Gnss, NOW + 5 * US, NOW), Verdict::Slew);
        assert_eq!(judge(ntp_long_ago, true, Source::Gnss, NOW + 1, NOW), Verdict::Confirm);
    }

    #[test]
    fn a_record_from_the_future_does_not_hold_the_clock() {
        // The clock was stepped back after the record was made.
        let ahead = Some(Sync { source: Source::Ntp, at_us: NOW + 3600 * US });
        assert_eq!(judge(ahead, true, Source::Gnss, NOW + 5 * US, NOW), Verdict::Slew);
    }

    #[test]
    fn a_kept_clock_gives_way_to_any_source() {
        let kept = synced(Source::Kept, 10);
        assert_eq!(judge(kept, true, Source::Rtc, NOW + 3 * US, NOW), Verdict::Slew);
        assert_eq!(judge(None, true, Source::Rtc, NOW + 1, NOW), Verdict::Confirm);
    }

    #[test]
    fn with_automatic_time_off_only_the_user_and_a_lost_clock_chip_set_it() {
        let manual = synced(Source::Manual, 10);
        assert_eq!(judge(manual, false, Source::Ntp, NOW + 60 * US, NOW), Verdict::Ignore);
        assert_eq!(judge(manual, false, Source::Gnss, NOW + 60 * US, NOW), Verdict::Ignore);
        assert_eq!(judge(manual, false, Source::Rtc, NOW + 60 * US, NOW), Verdict::Ignore);
        assert_eq!(judge(None, false, Source::Rtc, NOW, UNSET), Verdict::Step);
        assert_eq!(judge(manual, false, Source::Manual, NOW + 60 * US, NOW), Verdict::Step);
    }

    #[test]
    fn a_time_set_by_hand_gives_way_once_automatic_time_is_back_on() {
        let manual = synced(Source::Manual, 10);
        assert_eq!(judge(manual, true, Source::Network, NOW + 60 * US, NOW), Verdict::Step);
    }

    #[test]
    fn the_calendar_round_trips_and_agrees_with_known_dates() {
        assert_eq!(days_from_civil(1970, 1, 1), 0);
        assert_eq!(days_from_civil(2026, 1, 1) * 86_400, FLOOR_S);
        assert_eq!(days_from_civil(2100, 1, 1) * 86_400, CEILING_S);
        assert_eq!(civil_from_days(days_from_civil(2028, 2, 29)), (2028, 2, 29));
        for days in (-1000..80_000).step_by(37) {
            let (y, m, d) = civil_from_days(days);
            assert_eq!(days_from_civil(y, m, d), days);
        }
        let t = Civil::from_utc_seconds(NOW / US);
        assert_eq!(t, Civil::new(2026, 10, 13, 12, 0, 0).unwrap());
        assert_eq!(t.weekday(), 2, "13 October 2026 is a Tuesday");
        assert_eq!(t.as_utc_seconds(), NOW / US);
    }

    #[test]
    fn a_date_out_of_range_for_its_month_is_no_date() {
        assert!(Civil::new(2026, 2, 29, 0, 0, 0).is_none());
        assert!(Civil::new(2028, 2, 29, 0, 0, 0).is_some());
        assert!(Civil::new(2026, 4, 31, 0, 0, 0).is_none());
        assert!(Civil::new(2026, 13, 1, 0, 0, 0).is_none());
        assert!(Civil::new(2026, 1, 1, 24, 0, 0).is_none());
    }

    #[test]
    fn a_typed_time_is_read_with_or_without_seconds() {
        let expect = Civil::new(2026, 10, 13, 14, 5, 0).unwrap();
        assert_eq!(parse_typed("2026-10-13 14:05"), Some(expect));
        assert_eq!(parse_typed(" 2026-10-13T14:05 "), Some(expect));
        assert_eq!(parse_typed("2026-10-13 14:05:09").unwrap().second, 9);
        assert_eq!(parse_typed("2026-10-13 9:05").unwrap().hour, 9);
        for bad in ["", "2026-10-13", "14:05", "13.10.2026 14:05", "2026-10-32 14:05",
                    "2026-10-13 14:60", "2026-10-13 14:05:00:00", "26-10-13 14:05"] {
            assert_eq!(parse_typed(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn a_network_offset_becomes_a_fixed_posix_zone() {
        assert_eq!(posix_for_offset(0), "UTC0");
        assert_eq!(posix_for_offset(120), "<+02>-2");
        assert_eq!(posix_for_offset(330), "<+0530>-5:30");
        assert_eq!(posix_for_offset(-210), "<-0330>3:30");
        assert_eq!(posix_for_offset(-600), "<-10>10");
        assert_eq!(offset_label(120), "UTC+02:00");
        assert_eq!(offset_label(-210), "UTC-03:30");
        assert_eq!(offset_label(0), "UTC");
        assert!(offset_is_valid(345));
        assert!(!offset_is_valid(50));
        assert!(!offset_is_valid(15 * 60));
    }

    #[test]
    fn the_clock_face_follows_the_format() {
        assert_eq!(clock_face(14, 5, true), "14:05");
        assert_eq!(clock_face(0, 7, true), "00:07");
        assert_eq!(clock_face(14, 5, false), "2:05 PM");
        assert_eq!(clock_face(0, 7, false), "12:07 AM");
        assert_eq!(clock_face(12, 0, false), "12:00 PM");
    }

    #[test]
    fn settings_says_what_set_the_clock_and_when() {
        assert_eq!(describe(false, None, UNSET), "Not set yet");
        assert_eq!(describe(true, synced(Source::Ntp, 20), NOW), "Set by NTP, just now");
        assert_eq!(describe(true, synced(Source::Gnss, 300), NOW), "Set by GPS, 5 min ago");
        assert_eq!(describe(true, synced(Source::Network, 3 * 3600), NOW), "Set by the mobile network, 3 h ago");
        assert_eq!(describe(true, synced(Source::Rtc, 3 * 86_400), NOW), "Set by the clock chip, 3 days ago");
        assert_eq!(describe(true, synced(Source::Manual, 3 * 86_400), NOW), "Set by hand");
        assert_eq!(describe(true, None, NOW), "Kept since before the last restart");
        assert_eq!(describe(true, synced(Source::Kept, 5), NOW), "Kept since before the last restart");
    }
}
