// SPDX-License-Identifier: BSD-3-Clause
//! Tailnet chat's call rules, with no I/O (protocol.md section 4.8, the
//! spec's Calls): one call at a time, an offer answered busy while one is up
//! or a cellular call has the device, a stranger's declined, a cellular call
//! that rings ends a tailnet call, an SMS that the modem's RI announces waits
//! while a tailnet call runs over LTE, and a call ends after 10 s without a
//! frame. tailnet_chat.rs runs them; tailnet_call.rs carries the audio.

#[cfg(test)]
use crate::tailnet_chat_wire::Key;

/// A connected call that hears nothing for this long is ended, `timeout`.
pub const SILENCE_MS: u64 = 10_000;
/// An offer rings, or waits for its answer, this long at most.
pub const RING_MS: u64 = 45_000;
/// A call to a contact with no open connection has this long to dial it
/// and finish the handshake; then it is `unreachable`.
pub const REACH_MS: u64 = 7_000;
/// The one media transport v1 has.
pub const WS_OPUS: &str = "ws-opus";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Phase {
    /// Ours, dialling the contact before the offer can go.
    Reaching,
    /// Ours, offered; waiting for the answer.
    Offered,
    /// Theirs, ringing here.
    Ringing,
    Connected,
    /// Over: `ended:<reason>` or `declined:<reason>`, as the control port
    /// and the log say it.
    Over(String),
}

/// How the call log has a call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LogKind {
    Outgoing,
    Incoming,
    Missed,
    Rejected,
}

#[derive(Clone, Debug)]
pub struct Call<K> {
    pub id: String,
    pub key: K,
    pub mine: bool,
    pub stream: u32,
    pub phase: Phase,
    pub began_ms: u64,
    pub connected_ms: Option<u64>,
    pub ended_ms: Option<u64>,
    pub last_frame_ms: u64,
    /// Audio frames received.
    pub frames: u32,
    /// The user here declined it while it rang.
    pub declined_here: bool,
}

impl<K> Call<K> {
    pub fn new(id: String, key: K, mine: bool, stream: u32, phase: Phase, now: u64) -> Call<K> {
        Call {
            id,
            key,
            mine,
            stream,
            phase,
            began_ms: now,
            connected_ms: None,
            ended_ms: None,
            last_frame_ms: now,
            frames: 0,
            declined_here: false,
        }
    }

    /// Ringing, being placed, or connected.
    pub fn live(&self) -> bool {
        !matches!(self.phase, Phase::Over(_))
    }

    /// What the control port and the log call its state.
    pub fn state(&self) -> String {
        match &self.phase {
            Phase::Reaching => "reaching".into(),
            Phase::Offered => "offered".into(),
            Phase::Ringing => "ringing".into(),
            Phase::Connected => "connected".into(),
            Phase::Over(how) => how.clone(),
        }
    }

    pub fn connect(&mut self, now: u64) {
        if self.live() {
            self.phase = Phase::Connected;
            self.connected_ms = Some(now);
            self.last_frame_ms = now;
        }
    }

    /// An audio frame came.
    pub fn heard(&mut self, now: u64) {
        self.frames = self.frames.saturating_add(1);
        self.last_frame_ms = now;
    }

    /// Over, `how` being `ended:<reason>` or `declined:<reason>`. A call
    /// already over stays as it ended.
    pub fn end(&mut self, how: &str, now: u64) {
        if self.live() {
            self.phase = Phase::Over(how.to_string());
            self.ended_ms = Some(now);
        }
    }

    /// Seconds connected.
    pub fn duration_s(&self, now: u64) -> u32 {
        match self.connected_ms {
            Some(c) => ((self.ended_ms.unwrap_or(now).saturating_sub(c)) / 1000) as u32,
            None => 0,
        }
    }

    /// Whether time has ended it, and with what reason: an unreachable
    /// contact, an offer nobody answered, a connected call gone silent.
    pub fn due(&self, now: u64) -> Option<&'static str> {
        let since = |t: u64| now.saturating_sub(t);
        match self.phase {
            Phase::Reaching if since(self.began_ms) >= REACH_MS => Some("unreachable"),
            Phase::Offered | Phase::Ringing if since(self.began_ms) >= RING_MS => Some("timeout"),
            Phase::Connected if since(self.last_frame_ms) >= SILENCE_MS => Some("timeout"),
            _ => None,
        }
    }

    /// How the call log has it once over.
    pub fn log_kind(&self) -> LogKind {
        if self.mine {
            LogKind::Outgoing
        } else if self.connected_ms.is_some() {
            LogKind::Incoming
        } else if self.declined_here {
            LogKind::Rejected
        } else {
            LogKind::Missed
        }
    }
}

/// What an offer is answered with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Answer {
    /// It rings here for the user.
    Ring,
    /// `call_answer` with `decline` and this reason, at once.
    Decline(&'static str),
}

/// An offer from a peer: `in_call` a tailnet call is up, `cellular` a
/// cellular call has the device, `contact` the key is a contact.
pub fn answer_offer(in_call: bool, cellular: bool, contact: bool, transports: &[&str]) -> Answer {
    if !contact {
        Answer::Decline("declined")
    } else if !transports.contains(&WS_OPUS) {
        Answer::Decline("unsupported")
    } else if in_call || cellular {
        Answer::Decline("busy")
    } else {
        Answer::Ring
    }
}

/// Why a call cannot be placed now.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// A call is up already, tailnet or cellular.
    Busy,
    NotAContact,
}

pub fn may_place(in_call: bool, cellular: bool, contact: bool) -> Result<(), Refusal> {
    if !contact {
        Err(Refusal::NotAContact)
    } else if in_call || cellular {
        Err(Refusal::Busy)
    } else {
        Ok(())
    }
}

/// Whether a cellular call that starts (ringing here or placed) ends the
/// tailnet call: always, while one is live. A phone call is never missed.
pub fn cellular_ends<K>(call: Option<&Call<K>>) -> bool {
    call.is_some_and(|c| c.live())
}

/// Whether SMS the modem has waiting are fetched later: during a cellular
/// call (the call has the modem), and during a tailnet call that runs over
/// LTE, which fetching would interrupt for seconds.
pub fn sms_waits(cellular_call: bool, tailnet_call_over_lte: bool) -> bool {
    cellular_call || tailnet_call_over_lte
}

#[cfg(test)]
mod tests {
    use super::*;

    const K: Key = [7; 32];

    fn ringing(now: u64) -> Call<Key> {
        Call::new("c1".into(), K, false, 2, Phase::Ringing, now)
    }

    #[test]
    fn a_contact_s_offer_rings_when_nothing_else_is_up() {
        assert_eq!(answer_offer(false, false, true, &["ws-opus"]), Answer::Ring);
        assert_eq!(answer_offer(false, false, true, &["webrtc", "ws-opus"]), Answer::Ring);
    }

    #[test]
    fn a_second_call_gets_busy() {
        assert_eq!(answer_offer(true, false, true, &["ws-opus"]), Answer::Decline("busy"));
    }

    #[test]
    fn a_cellular_call_up_makes_the_device_busy() {
        assert_eq!(answer_offer(false, true, true, &["ws-opus"]), Answer::Decline("busy"));
        assert_eq!(may_place(false, true, true), Err(Refusal::Busy));
    }

    #[test]
    fn a_stranger_s_call_is_declined_even_during_a_call() {
        assert_eq!(answer_offer(false, false, false, &["ws-opus"]), Answer::Decline("declined"));
        assert_eq!(answer_offer(true, false, false, &["ws-opus"]), Answer::Decline("declined"));
    }

    #[test]
    fn an_offer_with_no_transport_in_common_is_unsupported() {
        assert_eq!(answer_offer(false, false, true, &["webrtc"]), Answer::Decline("unsupported"));
        assert_eq!(answer_offer(false, false, true, &[]), Answer::Decline("unsupported"));
    }

    #[test]
    fn one_call_at_a_time_and_only_to_a_contact() {
        assert_eq!(may_place(false, false, true), Ok(()));
        assert_eq!(may_place(true, false, true), Err(Refusal::Busy));
        assert_eq!(may_place(false, false, false), Err(Refusal::NotAContact));
    }

    #[test]
    fn a_cellular_call_ends_a_live_tailnet_call_only() {
        let mut c = ringing(0);
        assert!(cellular_ends(Some(&c)));
        c.connect(10);
        assert!(cellular_ends(Some(&c)));
        c.end("ended:hangup", 20);
        assert!(!cellular_ends(Some(&c)));
        assert!(!cellular_ends::<Key>(None));
    }

    #[test]
    fn sms_wait_for_a_tailnet_call_over_lte_but_not_over_wifi() {
        assert!(sms_waits(false, true));
        assert!(!sms_waits(false, false));
        assert!(sms_waits(true, false));
    }

    #[test]
    fn ten_seconds_without_a_frame_ends_a_connected_call() {
        let mut c = ringing(0);
        c.connect(1_000);
        c.heard(5_000);
        assert_eq!(c.due(14_999), None);
        assert_eq!(c.due(15_000), Some("timeout"));
    }

    #[test]
    fn the_silence_counts_from_the_answer_if_nothing_came() {
        let mut c = ringing(0);
        c.connect(30_000);
        assert_eq!(c.due(39_999), None);
        assert_eq!(c.due(40_000), Some("timeout"));
    }

    #[test]
    fn an_unanswered_offer_gives_up_after_45_s() {
        let c = ringing(0);
        assert_eq!(c.due(44_999), None);
        assert_eq!(c.due(45_000), Some("timeout"));
        let mine = Call::new("c2".into(), K, true, 2, Phase::Offered, 0);
        assert_eq!(mine.due(45_000), Some("timeout"));
    }

    #[test]
    fn a_contact_not_reached_in_time_is_unreachable() {
        let c = Call::new("c3".into(), K, true, 2, Phase::Reaching, 0);
        assert_eq!(c.due(REACH_MS - 1), None);
        assert_eq!(c.due(REACH_MS), Some("unreachable"));
    }

    #[test]
    fn an_ended_call_keeps_how_it_ended_and_is_never_due() {
        let mut c = ringing(0);
        c.end("ended:cellular", 100);
        c.end("ended:hangup", 200);
        assert_eq!(c.state(), "ended:cellular");
        assert_eq!(c.due(1_000_000), None);
        assert!(!c.live());
    }

    #[test]
    fn the_call_log_has_missed_rejected_incoming_and_outgoing() {
        let mut missed = ringing(0);
        missed.end("ended:hangup", 5_000);
        assert_eq!(missed.log_kind(), LogKind::Missed);

        let mut rejected = ringing(0);
        rejected.declined_here = true;
        rejected.end("declined:declined", 5_000);
        assert_eq!(rejected.log_kind(), LogKind::Rejected);

        let mut answered = ringing(0);
        answered.connect(2_000);
        answered.end("ended:hangup", 65_000);
        assert_eq!(answered.log_kind(), LogKind::Incoming);
        assert_eq!(answered.duration_s(99_000), 63);

        let mut mine = Call::new("c4".into(), K, true, 2, Phase::Offered, 0);
        mine.end("declined:busy", 1_000);
        assert_eq!(mine.log_kind(), LogKind::Outgoing);
        assert_eq!(mine.duration_s(2_000), 0);
    }

    #[test]
    fn the_state_reads_as_the_control_port_says_it() {
        let mut c = Call::new("c5".into(), K, true, 2, Phase::Reaching, 0);
        assert_eq!(c.state(), "reaching");
        c.phase = Phase::Offered;
        assert_eq!(c.state(), "offered");
        c.connect(1);
        assert_eq!(c.state(), "connected");
        c.end("declined:busy", 2);
        assert_eq!(c.state(), "declined:busy");
    }
}
