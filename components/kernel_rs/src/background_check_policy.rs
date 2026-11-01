// SPDX-License-Identifier: BSD-3-Clause
//! When a background check runs: the decisions behind background_check.rs,
//! kept apart from the tasks, the clock and the network so a host can test
//! them.

use std::time::{Duration, Instant};

/// A check's handle, given at registration.
pub type CheckId = u32;

/// One registered check, as the schedule sees it.
#[derive(Clone, Debug)]
struct Check {
    id: CheckId,
    /// None: off.
    interval: Option<Duration>,
    /// Internal RAM the check needs free to start, in bytes.
    ram_needed: usize,
    /// When it last ended, or was registered: it is due an interval after.
    since: Instant,
    /// Started and not yet finished.
    running: bool,
    /// Its module has it paused.
    paused: bool,
    /// Not to run before this time, in seconds since 1970.
    stopped_until: Option<i64>,
    /// Due, and waiting for a network or the clock since then.
    waiting_since: Option<Instant>,
}

impl Check {
    /// Not to run again before an interval from `now`.
    fn put_off(&mut self, now: Instant) {
        self.waiting_since = None;
        self.since = now;
    }

    /// When its interval has passed, if it is on and nothing holds it back
    /// but the stop.
    fn next_at(&self) -> Option<Instant> {
        if self.running || self.paused {
            return None;
        }
        self.interval.map(|every| self.since + every)
    }

    /// Whether a stop it is under has passed. With the clock unset nobody
    /// can tell.
    fn stop_over(&self, wall_s: Option<i64>) -> bool {
        self.stopped_until
            .is_none_or(|until| wall_s.is_some_and(|now| now >= until))
    }

    fn due(&self, now: Instant, wall_s: Option<i64>) -> bool {
        self.next_at().is_some_and(|at| now >= at) && self.stop_over(wall_s)
    }
}

/// Every registered check, and when each is due.
#[derive(Debug, Default)]
pub(crate) struct Schedule {
    checks: Vec<Check>,
    next_id: CheckId,
}

/// What the rest of the system is like right now.
#[derive(Clone, Copy, Debug)]
pub(crate) struct World {
    pub(crate) now: Instant,
    /// The time, in seconds since 1970, if the clock is set.
    pub(crate) wall_s: Option<i64>,
    /// A network is up.
    pub(crate) network: bool,
    /// Internal RAM free, in bytes.
    pub(crate) internal_free: usize,
}

/// What to do with a check now.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Step {
    /// Start it, on a task of its own.
    Start(CheckId),
    /// It was due, but cannot run: not until an interval from now.
    Skip(CheckId, Skip),
}

/// Why a due check did not run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Skip {
    NoNetwork,
    ClockUnset,
    LowRam { free: usize, needed: usize },
}

/// How long a due check waits for a network, or for the clock, before it is
/// skipped. A device woken from light sleep for a check needs this long to
/// bring WiFi or mobile data back; NTP sets an unset clock soon after.
pub(crate) const NETWORK_WAIT: Duration = Duration::from_secs(90);

/// How a check went, as it says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// It did its work; what it found, for the log.
    Done(String),
    /// It did not run, and why.
    Skipped(String),
    /// It ran and failed, and why. It is tried again at its next interval.
    Failed(String),
    /// It is not to run again before this time, in seconds since 1970.
    StoppedUntil(i64),
}

/// A time of 0 is off.
fn interval(d: Duration) -> Option<Duration> {
    (!d.is_zero()).then_some(d)
}

impl Schedule {
    /// Add a check, first due one interval from `now`.
    pub(crate) fn register(&mut self, every: Duration, ram_needed: usize, now: Instant) -> CheckId {
        self.next_id += 1;
        let id = self.next_id;
        self.checks.push(Check {
            id,
            interval: interval(every),
            ram_needed,
            since: now,
            running: false,
            paused: false,
            stopped_until: None,
            waiting_since: None,
        });
        id
    }

    fn get(&mut self, id: CheckId) -> Option<&mut Check> {
        self.checks.iter_mut().find(|c| c.id == id)
    }

    /// Change how often a check runs, counted from its last run; 0 is off.
    /// False if there is no such check.
    pub(crate) fn set_interval(&mut self, id: CheckId, every: Duration) -> bool {
        self.get(id).map(|c| c.interval = interval(every)).is_some()
    }

    /// Hold a check back, or let it run again. False if there is no such
    /// check.
    pub(crate) fn set_paused(&mut self, id: CheckId, paused: bool) -> bool {
        self.get(id)
            .map(|c| {
                c.paused = paused;
                c.waiting_since = None;
            })
            .is_some()
    }

    /// The checks to start or skip now. A check started is running until
    /// `finished` is called for it.
    pub(crate) fn poll(&mut self, w: &World) -> Vec<Step> {
        let mut steps = Vec::new();
        for c in &mut self.checks {
            if !c.due(w.now, w.wall_s) {
                continue;
            }
            // Waiting would not free any: skipped at once.
            if w.internal_free < c.ram_needed {
                c.put_off(w.now);
                steps.push(Step::Skip(c.id, Skip::LowRam { free: w.internal_free, needed: c.ram_needed }));
                continue;
            }
            let missing = if !w.network {
                Some(Skip::NoNetwork)
            } else if w.wall_s.is_none() {
                Some(Skip::ClockUnset)
            } else {
                None
            };
            match missing {
                None => {
                    c.waiting_since = None;
                    c.running = true;
                    steps.push(Step::Start(c.id));
                }
                Some(why) => {
                    let since = *c.waiting_since.get_or_insert(w.now);
                    if w.now.duration_since(since) >= NETWORK_WAIT {
                        c.put_off(w.now);
                        steps.push(Step::Skip(c.id, why));
                    }
                }
            }
        }
        steps
    }

    /// A check is running, or is due and waiting to: the device should
    /// stay up for it.
    pub(crate) fn busy(&self, now: Instant, wall_s: Option<i64>) -> bool {
        self.checks.iter().any(|c| c.running || c.due(now, wall_s))
    }

    /// When the earliest check falls due, for a light sleep to wake at: the
    /// power manager asks. None if no check is waiting to run.
    pub(crate) fn next_wake(&self, now: Instant, wall_s: Option<i64>) -> Option<Instant> {
        self.checks
            .iter()
            .filter_map(|c| {
                let at = c.next_at()?;
                match c.stopped_until {
                    None => Some(at),
                    Some(until) => {
                        let wall = wall_s?;
                        let left = Duration::from_secs(until.saturating_sub(wall).max(0) as u64);
                        Some(at.max(now + left))
                    }
                }
            })
            .min()
    }

    /// A check that was started has ended, with `outcome`.
    pub(crate) fn finished(&mut self, id: CheckId, outcome: &Outcome, w: &World) {
        let Some(c) = self.get(id) else {
            return;
        };
        c.running = false;
        c.put_off(w.now);
        if let Outcome::StoppedUntil(until) = *outcome {
            c.stopped_until = Some(until);
        } else {
            c.stopped_until = None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KB: usize = 1024;

    fn mins(m: u64) -> Duration {
        Duration::from_secs(m * 60)
    }

    /// A world with a network, the clock set and plenty of RAM, `at` after t0.
    fn world(t0: Instant, at: Duration) -> World {
        World {
            now: t0 + at,
            wall_s: Some(1_800_000_000 + at.as_secs() as i64),
            network: true,
            internal_free: 150 * KB,
        }
    }

    #[test]
    fn a_check_is_due_once_its_interval_has_passed() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        assert_eq!(s.poll(&world(t0, mins(14))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(15))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_running_check_is_not_started_again_and_is_next_due_an_interval_after_it_ends() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        assert_eq!(s.poll(&world(t0, mins(15))), vec![Step::Start(id)]);
        assert_eq!(s.poll(&world(t0, mins(16))), vec![]);
        s.finished(id, &Outcome::Done(String::new()), &world(t0, mins(17)));
        assert_eq!(s.poll(&world(t0, mins(31))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(32))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_check_is_skipped_with_too_little_internal_ram_free_and_tried_an_interval_later() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        let mut w = world(t0, mins(15));
        w.internal_free = 59 * KB;
        assert_eq!(
            s.poll(&w),
            vec![Step::Skip(id, Skip::LowRam { free: 59 * KB, needed: 60 * KB })]
        );
        assert_eq!(s.poll(&world(t0, mins(29))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(30))), vec![Step::Start(id)]);
    }

    #[test]
    fn with_no_network_a_due_check_waits_for_one_then_is_skipped() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        let offline = |at| World { network: false, ..world(t0, at) };
        assert_eq!(s.poll(&offline(mins(15))), vec![]);
        assert!(s.busy(t0 + mins(15), None), "waiting for a network keeps it busy");
        let gave_up = mins(15) + NETWORK_WAIT;
        assert_eq!(s.poll(&offline(gave_up)), vec![Step::Skip(id, Skip::NoNetwork)]);
        assert!(!s.busy(t0 + gave_up, None));
        assert_eq!(s.poll(&world(t0, gave_up + mins(14))), vec![]);
        assert_eq!(s.poll(&world(t0, gave_up + mins(15))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_network_that_comes_while_it_waits_starts_it() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        let offline = World { network: false, ..world(t0, mins(15)) };
        assert_eq!(s.poll(&offline), vec![]);
        let later = world(t0, mins(15) + Duration::from_secs(20));
        assert_eq!(s.poll(&later), vec![Step::Start(id)]);
    }

    #[test]
    fn with_the_clock_unset_a_due_check_waits_for_it_then_is_skipped() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        let unset = |at| World { wall_s: None, ..world(t0, at) };
        assert_eq!(s.poll(&unset(mins(15))), vec![]);
        assert_eq!(
            s.poll(&unset(mins(15) + NETWORK_WAIT)),
            vec![Step::Skip(id, Skip::ClockUnset)]
        );
    }

    #[test]
    fn a_paused_check_is_not_run_and_runs_once_resumed() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        s.set_paused(id, true);
        assert_eq!(s.poll(&world(t0, mins(20))), vec![]);
        assert!(!s.busy(t0 + mins(20), None));
        s.set_paused(id, false);
        assert_eq!(s.poll(&world(t0, mins(21))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_check_stopped_until_a_time_is_not_run_before_it() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        assert_eq!(s.poll(&world(t0, mins(15))), vec![Step::Start(id)]);
        let until = world(t0, mins(120)).wall_s.unwrap();
        s.finished(id, &Outcome::StoppedUntil(until), &world(t0, mins(15)));
        assert_eq!(s.poll(&world(t0, mins(30))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(119))), vec![]);
        assert!(!s.busy(t0 + mins(119), world(t0, mins(119)).wall_s));
        assert_eq!(s.poll(&world(t0, mins(120))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_check_stopped_until_a_time_waits_for_the_clock_to_know_it_has_passed() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        s.poll(&world(t0, mins(15)));
        s.finished(id, &Outcome::StoppedUntil(0), &world(t0, mins(15)));
        let unset = World { wall_s: None, ..world(t0, mins(60)) };
        assert_eq!(s.poll(&unset), vec![]);
        assert_eq!(s.poll(&unset), vec![], "not skipped either: it is not due");
    }

    #[test]
    fn an_interval_of_0_is_off() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let off = s.register(Duration::ZERO, 60 * KB, t0);
        let on = s.register(mins(15), 60 * KB, t0);
        assert_eq!(s.poll(&world(t0, mins(15))), vec![Step::Start(on)]);
        s.set_interval(on, Duration::ZERO);
        s.finished(on, &Outcome::Done(String::new()), &world(t0, mins(15)));
        assert_eq!(s.poll(&world(t0, mins(600))), vec![]);
        assert!(!s.busy(t0 + mins(600), None));
        let _ = off;
    }

    #[test]
    fn a_new_interval_counts_from_the_last_run() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        s.poll(&world(t0, mins(15)));
        s.finished(id, &Outcome::Done(String::new()), &world(t0, mins(15)));
        s.set_interval(id, mins(5));
        assert_eq!(s.poll(&world(t0, mins(19))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(20))), vec![Step::Start(id)]);
    }

    #[test]
    fn a_failed_check_is_tried_again_an_interval_later_not_sooner() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        s.poll(&world(t0, mins(15)));
        s.finished(id, &Outcome::Failed("refused".into()), &world(t0, mins(16)));
        assert_eq!(s.poll(&world(t0, mins(30))), vec![]);
        assert_eq!(s.poll(&world(t0, mins(31))), vec![Step::Start(id)]);
    }

    // ── The next wake ───────────────────────────────────────────────────

    #[test]
    fn the_next_wake_is_the_earliest_due_check() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        assert_eq!(s.next_wake(t0, None), None, "no checks, no wake");
        s.register(mins(30), 60 * KB, t0);
        s.register(mins(5), 60 * KB, t0);
        s.register(mins(15), 60 * KB, t0);
        let w = world(t0, mins(1));
        assert_eq!(s.next_wake(w.now, w.wall_s), Some(t0 + mins(5)));
    }

    #[test]
    fn checks_that_are_off_or_paused_wake_nothing() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        s.register(Duration::ZERO, 60 * KB, t0);
        let paused = s.register(mins(5), 60 * KB, t0);
        s.set_paused(paused, true);
        let w = world(t0, mins(1));
        assert_eq!(s.next_wake(w.now, w.wall_s), None);
    }

    #[test]
    fn a_stopped_check_wakes_the_device_when_its_stop_ends() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        let id = s.register(mins(15), 60 * KB, t0);
        s.poll(&world(t0, mins(15)));
        let until = world(t0, mins(120)).wall_s.unwrap();
        s.finished(id, &Outcome::StoppedUntil(until), &world(t0, mins(15)));
        let w = world(t0, mins(20));
        assert_eq!(s.next_wake(w.now, w.wall_s), Some(t0 + mins(120)));
        assert_eq!(s.next_wake(w.now, None), None, "with the clock unset, nobody can tell when");
    }

    #[test]
    fn a_check_already_due_wakes_the_device_at_once() {
        let t0 = Instant::now();
        let mut s = Schedule::default();
        s.register(mins(5), 60 * KB, t0);
        let w = world(t0, mins(7));
        assert_eq!(s.next_wake(w.now, w.wall_s), Some(t0 + mins(5)));
    }
}
