// SPDX-License-Identifier: BSD-3-Clause
//! What the tailnet should do next, decided without touching it.
//!
//! The device joins the user's tailnet (Tailscale) while they have it
//! switched on, and while something can carry it:
//!
//! - It needs a network, WiFi or mobile data, and it starts again whenever
//!   that changes, so the client's sockets are never left on an interface
//!   that has gone. Switching between WiFi and mobile data is a change.
//! - It needs the clock: a WireGuard handshake carries the time, and a peer
//!   refuses one that is older than the last it saw.
//! - A device that has never joined signs in at a URL the control plane
//!   gives, shown as a QR code. Nothing secret is compiled in or stored
//!   but the device's own identity.
//! - A refusal stops it until the user switches it off and on. Anything
//!   else that stops the client is retried with backoff.
//!
//! Times are milliseconds since boot.

/// What a tailnet needs carried: the underlay's kind (hal_net_transport_t)
/// and its address. A change in either is a new network.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Underlay {
    pub kind: u32,
    pub ip: String,
}

/// microlink_state_t, as tailnet_platform_state() reports it; -1 is a
/// client that is not running, or busy starting or stopping.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Client {
    NotRunning,
    Idle,
    Connecting,
    Connected,
    Error,
    NeedsLogin,
    NeedsApproval,
}

impl Client {
    pub(crate) fn from_raw(state: i32) -> Self {
        match state {
            0 => Client::Idle,
            1..=3 | 5 => Client::Connecting, // WIFI_WAIT, CONNECTING, REGISTERING, RECONNECTING
            4 => Client::Connected,
            6 => Client::Error,
            7 => Client::NeedsLogin,
            8 => Client::NeedsApproval,
            _ => Client::NotRunning,
        }
    }
}

/// microlink_error_t
pub(crate) const ERROR_REFUSED: i32 = 1;
pub(crate) const ERROR_NODE_KEY_EXPIRED: i32 = 2;

/// What the manager knows when it looks, once a second.
#[derive(Clone, Debug)]
pub(crate) struct Inputs {
    pub enabled: bool,
    pub underlay: Option<Underlay>,
    pub clock_known: bool,
    /// The underlay the running client was started on; None if not running.
    pub running_on: Option<Underlay>,
    pub client: Client,
    pub client_error: i32,
    /// A starting or stopping is under way, on its own task.
    pub busy: bool,
    /// Stopped by a refusal, until switched off and on.
    pub refused: bool,
    pub now_ms: u64,
    /// No start before this, after a failure.
    pub retry_at_ms: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    None,
    Start,
    Stop,
    /// Stop, then start: a new network.
    Restart,
    /// Stop and stay stopped: the control plane refused the registration.
    StopRefused,
    /// Stop, and start again after a backoff: the client gave up for some
    /// other reason. An expired node key is one: the client has saved a new
    /// one, and the next start signs in with it.
    StopFailed,
}

pub(crate) fn decide(i: &Inputs) -> Action {
    if i.busy {
        return Action::None;
    }
    let running = i.running_on.is_some();
    if !i.enabled {
        return if running { Action::Stop } else { Action::None };
    }
    let Some(underlay) = &i.underlay else {
        return if running { Action::Stop } else { Action::None };
    };
    if !running {
        if !i.clock_known || i.now_ms < i.retry_at_ms {
            return Action::None;
        }
        if i.refused {
            return Action::None;
        }
        return Action::Start;
    }
    if i.running_on.as_ref() != Some(underlay) {
        return Action::Restart;
    }
    match i.client {
        Client::Error if i.client_error == ERROR_REFUSED => Action::StopRefused,
        Client::Error => Action::StopFailed,
        _ => Action::None,
    }
}

/// tailnet_state_t, for the UI.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum Shown {
    Off = 0,
    NoNetwork = 1,
    NoClock = 2,
    Connecting = 3,
    NeedsLogin = 4,
    NeedsApproval = 5,
    Connected = 6,
    Failed = 7,
    Unavailable = 8,
}

pub(crate) fn shown(available: bool, i: &Inputs, failure: bool) -> Shown {
    if !available {
        return Shown::Unavailable;
    }
    if !i.enabled {
        return Shown::Off;
    }
    if i.refused {
        return Shown::Failed;
    }
    if i.underlay.is_none() {
        return Shown::NoNetwork;
    }
    if i.running_on.is_none() {
        if !i.clock_known {
            return Shown::NoClock;
        }
        return if failure { Shown::Failed } else { Shown::Connecting };
    }
    match i.client {
        Client::Connected => Shown::Connected,
        Client::NeedsLogin => Shown::NeedsLogin,
        Client::NeedsApproval => Shown::NeedsApproval,
        Client::Error => Shown::Failed,
        _ => Shown::Connecting,
    }
}

/// How long to wait before starting again after the client could not be
/// started: 10 s, doubling to 5 minutes.
pub(crate) fn backoff_ms(failures: u32) -> u64 {
    let base: u64 = 10_000;
    base.saturating_mul(1u64 << failures.min(5)).min(300_000)
}

/// A name the tailnet will take for this device: lower case letters, digits
/// and `-`, at most 63, not starting or ending with `-`. Anything else
/// becomes `-`; None if nothing is left.
pub(crate) fn hostname_from(name: &str) -> Option<String> {
    let mut out = String::with_capacity(name.len());
    for c in name.trim().chars() {
        let c = c.to_ascii_lowercase();
        if c.is_ascii_lowercase() || c.is_ascii_digit() {
            out.push(c);
        } else if !out.ends_with('-') {
            out.push('-');
        }
        if out.len() == 63 {
            break;
        }
    }
    let trimmed = out.trim_matches('-');
    (!trimmed.is_empty()).then(|| trimmed.to_string())
}

/// The words for a refusal, for the user: the control plane's own words.
pub(crate) fn refusal_text(why: &str) -> String {
    let why = why.trim();
    if why.is_empty() {
        "The tailnet refused this device".to_string()
    } else {
        format!("The tailnet refused this device: {why}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wifi() -> Underlay {
        Underlay { kind: 0, ip: "192.168.1.20".into() }
    }

    fn base() -> Inputs {
        Inputs {
            enabled: true,
            underlay: Some(wifi()),
            clock_known: true,
            running_on: None,
            client: Client::NotRunning,
            client_error: 0,
            busy: false,
            refused: false,
            now_ms: 100_000,
            retry_at_ms: 0,
        }
    }

    fn running(client: Client) -> Inputs {
        Inputs { running_on: Some(wifi()), client, ..base() }
    }

    #[test]
    fn starts_when_on_with_a_network_and_the_clock() {
        assert_eq!(decide(&base()), Action::Start);
    }

    #[test]
    fn waits_for_a_network_and_for_the_clock() {
        assert_eq!(decide(&Inputs { underlay: None, ..base() }), Action::None);
        assert_eq!(decide(&Inputs { clock_known: false, ..base() }), Action::None);
        assert_eq!(shown(true, &Inputs { underlay: None, ..base() }, false), Shown::NoNetwork);
        assert_eq!(shown(true, &Inputs { clock_known: false, ..base() }, false), Shown::NoClock);
    }

    #[test]
    fn does_nothing_while_starting_or_stopping() {
        assert_eq!(decide(&Inputs { busy: true, ..base() }), Action::None);
        assert_eq!(decide(&Inputs { busy: true, enabled: false, ..running(Client::Connected) }),
                   Action::None);
    }

    #[test]
    fn stops_when_switched_off_or_the_network_goes() {
        assert_eq!(decide(&Inputs { enabled: false, ..running(Client::Connected) }), Action::Stop);
        assert_eq!(decide(&Inputs { underlay: None, ..running(Client::Connected) }), Action::Stop);
        assert_eq!(decide(&Inputs { enabled: false, ..base() }), Action::None);
        assert_eq!(shown(true, &Inputs { enabled: false, ..base() }, false), Shown::Off);
    }

    #[test]
    fn starts_again_on_a_new_network() {
        let lte = Underlay { kind: 1, ip: "10.20.30.40".into() };
        assert_eq!(decide(&Inputs { underlay: Some(lte), ..running(Client::Connected) }),
                   Action::Restart);
        let new_ip = Underlay { kind: 0, ip: "192.168.1.21".into() };
        assert_eq!(decide(&Inputs { underlay: Some(new_ip), ..running(Client::Connected) }),
                   Action::Restart);
        assert_eq!(decide(&running(Client::Connected)), Action::None);
    }

    #[test]
    fn a_refusal_stops_it_until_switched_off_and_on() {
        let refused = Inputs { client_error: ERROR_REFUSED, ..running(Client::Error) };
        assert_eq!(decide(&refused), Action::StopRefused);
        let after = Inputs { refused: true, ..base() };
        assert_eq!(decide(&after), Action::None);
        assert_eq!(shown(true, &after, false), Shown::Failed);
    }

    #[test]
    fn an_expired_node_key_signs_in_again() {
        let expired = Inputs { client_error: ERROR_NODE_KEY_EXPIRED, ..running(Client::Error) };
        assert_eq!(decide(&expired), Action::StopFailed);
    }

    #[test]
    fn waits_on_a_sign_in_without_starting_again() {
        assert_eq!(decide(&running(Client::NeedsLogin)), Action::None);
        assert_eq!(decide(&running(Client::NeedsApproval)), Action::None);
    }

    #[test]
    fn a_failed_start_waits_its_backoff() {
        let waiting = Inputs { retry_at_ms: 110_000, ..base() };
        assert_eq!(decide(&waiting), Action::None);
        assert_eq!(shown(true, &waiting, true), Shown::Failed);
        assert_eq!(decide(&Inputs { now_ms: 110_000, ..waiting }), Action::Start);
        assert_eq!(backoff_ms(0), 10_000);
        assert_eq!(backoff_ms(1), 20_000);
        assert_eq!(backoff_ms(4), 160_000);
        assert_eq!(backoff_ms(5), 300_000);
        assert_eq!(backoff_ms(40), 300_000);
    }

    #[test]
    fn shows_what_the_client_is_doing() {
        assert_eq!(shown(true, &running(Client::Connecting), false), Shown::Connecting);
        assert_eq!(shown(true, &running(Client::NeedsLogin), false), Shown::NeedsLogin);
        assert_eq!(shown(true, &running(Client::NeedsApproval), false), Shown::NeedsApproval);
        assert_eq!(shown(true, &running(Client::Connected), false), Shown::Connected);
        assert_eq!(shown(false, &running(Client::Connected), false), Shown::Unavailable);
        assert_eq!(shown(true, &base(), false), Shown::Connecting);
    }

    #[test]
    fn client_states_map_from_microlink() {
        assert_eq!(Client::from_raw(-1), Client::NotRunning);
        assert_eq!(Client::from_raw(0), Client::Idle);
        assert_eq!(Client::from_raw(3), Client::Connecting);
        assert_eq!(Client::from_raw(4), Client::Connected);
        assert_eq!(Client::from_raw(5), Client::Connecting);
        assert_eq!(Client::from_raw(6), Client::Error);
        assert_eq!(Client::from_raw(7), Client::NeedsLogin);
        assert_eq!(Client::from_raw(8), Client::NeedsApproval);
        assert_eq!(Client::from_raw(99), Client::NotRunning);
    }

    #[test]
    fn makes_a_hostname() {
        assert_eq!(hostname_from("thistle-a1b2c3").as_deref(), Some("thistle-a1b2c3"));
        assert_eq!(hostname_from("My T-Deck Max").as_deref(), Some("my-t-deck-max"));
        assert_eq!(hostname_from("  --odd__name!!  ").as_deref(), Some("odd-name"));
        assert_eq!(hostname_from("Ölbaum").as_deref(), Some("lbaum"));
        assert_eq!(hostname_from("!!!"), None);
        assert_eq!(hostname_from(""), None);
        assert_eq!(hostname_from(&"a".repeat(100)).map(|h| h.len()), Some(63));
    }

    #[test]
    fn says_what_was_refused() {
        assert_eq!(refusal_text("node not allowed"), "The tailnet refused this device: node not allowed");
        assert_eq!(refusal_text(" "), "The tailnet refused this device");
    }
}
