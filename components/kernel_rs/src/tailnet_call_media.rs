// SPDX-License-Identifier: BSD-3-Clause
//! A tailnet call's audio decisions, with no I/O: the jitter buffer the far
//! end's frames wait in (60 to 200 ms, protocol.md section 4.8), and the
//! echo suppression that turns the microphone down while the far end is
//! loud. The device has no echo canceller (the spec leaves it out of v1):
//! its speaker is loud and close to its microphone, so the far end would
//! hear itself back. tailnet_call.rs runs both, one 20 ms frame at a time.

use std::collections::BTreeMap;

/// One frame: 20 ms at 16 kHz.
pub const FRAME_MS: u32 = 20;
pub const FRAME_SAMPLES: usize = 320;

/// What to play for the next 20 ms.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Pull {
    /// Decode this packet.
    Play(Vec<u8>),
    /// The packet is missing: the decoder conceals it.
    Conceal,
    /// Still filling: silence.
    Wait,
}

/// Packets arrive when the network lets them and leave one every 20 ms, in
/// sequence, after 60 to 200 ms in the buffer. A packet missing at its turn
/// is concealed; one arriving after its turn is dropped. Each time the
/// buffer runs dry it holds a frame more, up to the most.
pub struct Jitter {
    queue: BTreeMap<u32, Vec<u8>>,
    target: usize,
    max: usize,
    playing: bool,
    ran_dry: bool,
    next: Option<u32>,
    /// Packets that came after their turn.
    pub late: u32,
    /// Turns with no packet, concealed.
    pub concealed: u32,
    /// Packets dropped because the buffer was full.
    pub dropped: u32,
}

/// A sequence number this far from the one expected is a sender that
/// started again, not a late or early packet.
const RESTART: u32 = 50;

impl Jitter {
    pub fn new(target_ms: u32, max_ms: u32) -> Jitter {
        Jitter {
            queue: BTreeMap::new(),
            target: (target_ms / FRAME_MS) as usize,
            max: (max_ms / FRAME_MS) as usize,
            playing: false,
            ran_dry: false,
            next: None,
            late: 0,
            concealed: 0,
            dropped: 0,
        }
    }

    /// One that plays nothing until replaced.
    pub const fn empty() -> Jitter {
        Jitter {
            queue: BTreeMap::new(),
            target: 3,
            max: 10,
            playing: false,
            ran_dry: false,
            next: None,
            late: 0,
            concealed: 0,
            dropped: 0,
        }
    }

    /// The buffer the protocol asks for: 60 ms to start, 200 ms at most.
    pub fn call() -> Jitter {
        Jitter::new(60, 200)
    }

    /// How long audio waits before it plays now.
    pub fn delay_ms(&self) -> u32 {
        self.target as u32 * FRAME_MS
    }

    pub fn queued_ms(&self) -> u32 {
        self.queue.len() as u32 * FRAME_MS
    }

    pub fn push(&mut self, seq: u32, packet: Vec<u8>) {
        if let Some(next) = self.next {
            if seq.abs_diff(next) > RESTART {
                self.queue.clear();
                self.playing = false;
                self.next = None;
            } else if seq < next {
                self.late += 1;
                return;
            }
        }
        self.queue.insert(seq, packet);
        if self.queue.len() > self.max {
            while self.queue.len() > self.target {
                self.queue.pop_first();
                self.dropped += 1;
            }
            if self.playing {
                self.next = self.queue.keys().next().copied();
            }
        }
    }

    pub fn pull(&mut self) -> Pull {
        if !self.playing {
            if self.queue.len() < self.target.max(1) {
                return Pull::Wait;
            }
            self.playing = true;
            self.ran_dry = false;
            self.next = self.queue.keys().next().copied();
        }
        let next = self.next.unwrap_or(0);
        if let Some(packet) = self.queue.remove(&next) {
            self.next = Some(next.wrapping_add(1));
            return Pull::Play(packet);
        }
        self.concealed += 1;
        self.next = Some(next.wrapping_add(1));
        if self.queue.is_empty() {
            // Ran dry: conceal this turn, then fill again, holding a frame more.
            if !self.ran_dry {
                self.ran_dry = true;
                if self.target < self.max {
                    self.target += 1;
                }
            }
            self.playing = false;
        }
        Pull::Conceal
    }
}

/// The far end's level above which the microphone is turned down: about
/// -30 dBFS, speech at a normal level, not its background.
pub const FAR_LOUD_RMS: u32 = 1_000;
/// The microphone while the far end is loud: -20 dB.
pub const DUCKED_GAIN_Q8: u32 = 26;
/// How long it stays down after the far end goes quiet, for the speaker's
/// sound still in the room and the codec's delay.
pub const HANGOVER_MS: u32 = 200;

/// Echo suppression: the microphone is turned down while what the speaker
/// plays is loud, and for HANGOVER_MS after.
#[derive(Default)]
pub struct Ducker {
    hold_ms: u32,
    /// Frames sent turned down, for the log.
    pub ducked: u32,
}

impl Ducker {
    pub fn new() -> Ducker {
        Ducker::default()
    }

    /// The 20 ms just played to the speaker.
    pub fn played(&mut self, pcm: &[i16]) {
        if rms(pcm) >= FAR_LOUD_RMS {
            self.hold_ms = HANGOVER_MS;
        } else {
            self.hold_ms = self.hold_ms.saturating_sub(FRAME_MS);
        }
    }

    /// Whether the microphone is down now.
    pub fn down(&self) -> bool {
        self.hold_ms > 0
    }

    /// The 20 ms just recorded, turned down if the far end is loud.
    pub fn mic(&mut self, pcm: &mut [i16]) {
        if self.down() {
            self.ducked += 1;
            scale(pcm, DUCKED_GAIN_Q8);
        }
    }
}

/// Root mean square of the samples.
pub fn rms(pcm: &[i16]) -> u32 {
    if pcm.is_empty() {
        return 0;
    }
    let sum: u64 = pcm.iter().map(|&s| (s as i64 * s as i64) as u64).sum();
    ((sum / pcm.len() as u64) as f64).sqrt() as u32
}

/// Multiply by `q8`/256, clipped.
pub fn scale(pcm: &mut [i16], q8: u32) {
    for s in pcm.iter_mut() {
        let v = (*s as i64 * q8 as i64) >> 8;
        *s = v.clamp(i16::MIN as i64, i16::MAX as i64) as i16;
    }
}

/// The call's volume, 0-100 percent, as a gain on what is played: 70 %
/// (the default) plays as received, 100 % about 3 dB louder.
pub fn volume_q8(percent: u8) -> u32 {
    percent.min(100) as u32 * 256 / 70
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(n: u8) -> Vec<u8> {
        vec![n]
    }

    fn played(j: &mut Jitter) -> Vec<String> {
        let mut out = Vec::new();
        loop {
            match j.pull() {
                Pull::Play(x) => out.push(format!("{}", x[0])),
                Pull::Conceal => out.push("C".into()),
                Pull::Wait => return out,
            }
        }
    }

    #[test]
    fn nothing_plays_until_60_ms_are_buffered() {
        let mut j = Jitter::call();
        j.push(0, p(0));
        j.push(1, p(1));
        assert_eq!(j.pull(), Pull::Wait);
        j.push(2, p(2));
        assert_eq!(j.pull(), Pull::Play(p(0)));
        assert_eq!(j.delay_ms(), 60);
    }

    #[test]
    fn packets_play_in_sequence_whatever_order_they_came_in() {
        let mut j = Jitter::call();
        for s in [2, 0, 1, 4, 3] {
            j.push(s, p(s as u8));
        }
        assert_eq!(played(&mut j)[..5], ["0", "1", "2", "3", "4"]);
    }

    #[test]
    fn a_missing_packet_is_concealed_and_a_late_one_dropped() {
        let mut j = Jitter::call();
        for s in [0, 1, 3, 4] {
            j.push(s, p(s as u8));
        }
        assert_eq!(j.pull(), Pull::Play(p(0)));
        assert_eq!(j.pull(), Pull::Play(p(1)));
        assert_eq!(j.pull(), Pull::Conceal);
        j.push(2, p(2));
        assert_eq!(j.late, 1);
        assert_eq!(j.pull(), Pull::Play(p(3)));
        assert_eq!(j.concealed, 1);
    }

    #[test]
    fn running_dry_conceals_once_then_refills_holding_a_frame_more() {
        let mut j = Jitter::call();
        for s in 0..3 {
            j.push(s, p(s as u8));
        }
        assert_eq!(played(&mut j), ["0", "1", "2", "C"]);
        assert_eq!(j.delay_ms(), 80);
        for s in 4..7 {
            j.push(s, p(s as u8));
        }
        assert_eq!(j.pull(), Pull::Wait);
        j.push(7, p(7));
        assert_eq!(j.pull(), Pull::Play(p(4)));
    }

    #[test]
    fn it_never_holds_more_than_200_ms() {
        let mut j = Jitter::call();
        for s in 0..11 {
            j.push(s, p(s as u8));
        }
        assert!(j.queued_ms() <= 200, "{} ms queued", j.queued_ms());
        assert_eq!(j.pull(), Pull::Play(p(8)));
        assert_eq!(j.dropped, 8);
    }

    #[test]
    fn a_sender_that_starts_again_is_followed() {
        let mut j = Jitter::call();
        for s in 0..3 {
            j.push(s, p(s as u8));
        }
        j.pull();
        for s in 1000..1003 {
            j.push(s, p((s - 1000) as u8 + 100));
        }
        assert_eq!(j.pull(), Pull::Play(p(100)));
    }

    fn tone(level: i16) -> Vec<i16> {
        (0..FRAME_SAMPLES).map(|i| if i % 2 == 0 { level } else { -level }).collect()
    }

    #[test]
    fn the_microphone_goes_down_20_db_while_the_far_end_is_loud() {
        let mut d = Ducker::new();
        d.played(&tone(4000));
        let mut mic = tone(10_000);
        d.mic(&mut mic);
        assert_eq!(rms(&mic), 1015, "-20 dB is a tenth");
        assert_eq!(d.ducked, 1);
    }

    #[test]
    fn a_quiet_far_end_leaves_the_microphone_alone() {
        let mut d = Ducker::new();
        d.played(&tone(300));
        let mut mic = tone(10_000);
        d.mic(&mut mic);
        assert_eq!(rms(&mic), 10_000);
    }

    #[test]
    fn the_microphone_comes_back_200_ms_after_the_far_end_goes_quiet() {
        let mut d = Ducker::new();
        d.played(&tone(4000));
        for _ in 0..9 {
            d.played(&tone(0));
            assert!(d.down());
        }
        d.played(&tone(0));
        assert!(!d.down());
    }

    #[test]
    fn the_default_volume_plays_as_received() {
        assert_eq!(volume_q8(70), 256);
        assert_eq!(volume_q8(0), 0);
        assert!(volume_q8(100) > 256 && volume_q8(100) <= 384);
    }
}
