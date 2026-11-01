// SPDX-License-Identifier: BSD-3-Clause
//! Background checks, run: short pieces of work kernel modules register to
//! look for news while nothing in front asks for it (ADR-0014). When each is
//! due is decided in background_check_policy.rs; this runs them.
//!
//! - A kernel worker job looks once a second: which checks are due, and is
//!   there a network, the clock, the internal RAM each needs. A due check
//!   waits for a network and the clock, up to NETWORK_WAIT, and is then
//!   skipped; one that finds too little internal RAM free is skipped at once.
//!   Either is tried again an interval later.
//! - A check runs on a FreeRTOS task made for it and gone after
//!   (kernel_task::spawn), its stack in internal RAM, and holds the device
//!   awake while it runs. Each run is logged with its name, its outcome, how
//!   long it took and the internal RAM free before and after.
//! - The power manager asks when the next check falls and whether one is due
//!   or running: light sleep wakes on a timer for it, brings the network
//!   back with the screen left asleep, and sleeps again once it is over.
//!   Deep sleep runs none; waking from it is a restart.
//!
//! Only kernel modules register checks. Packaged apps cannot (there is no
//! syscall): their code would have to stay loaded and be called off the UI
//! task. The service knows nothing of what a check does.

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub use crate::background_check_policy::{CheckId, Outcome};
use crate::background_check_policy::{Schedule, Skip, Step, World};
use crate::power_manager::HoldGuard;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_FOUND: i32 = 0x105;

/// The smallest task stack a check is given.
const MIN_STACK: usize = 4096;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

fn log(level: i32, text: &str) {
    let text = CString::new(text.replace('\0', " ")).unwrap_or_default();
    #[cfg(not(test))]
    unsafe {
        esp_log_write(level, b"checks\0".as_ptr(), b"%s\0".as_ptr(), text.as_ptr());
    }
    #[cfg(test)]
    let _ = (level, text);
}

// ---------------------------------------------------------------------------
// The registered checks
// ---------------------------------------------------------------------------

type Run = Arc<dyn Fn() -> Outcome + Send + Sync>;

struct Entry {
    id: CheckId,
    /// Kept for good: it names the hold the check takes.
    name: &'static CStr,
    stack: usize,
    run: Run,
}

struct State {
    schedule: Schedule,
    entries: Vec<Entry>,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);

fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    let mut guard: MutexGuard<'_, Option<State>> = STATE.lock().unwrap_or_else(PoisonError::into_inner);
    let state = guard.get_or_insert_with(|| State {
        schedule: Schedule::default(),
        entries: Vec::new(),
    });
    f(state)
}

/// Register a check: `name` for the log, run every `every` (zero is off)
/// on a task with `stack` bytes of stack, once `ram_needed` bytes of
/// internal RAM, the stack among them, are free. It is first due one
/// interval from now. Kernel modules only.
pub fn register<F>(name: &str, every: Duration, ram_needed: usize, stack: usize, run: F) -> CheckId
where
    F: Fn() -> Outcome + Send + Sync + 'static,
{
    let name: &'static CStr = Box::leak(
        CString::new(name.replace('\0', " ")).unwrap_or_default().into_boxed_c_str(),
    );
    let id = with(|s| {
        let id = s.schedule.register(every, ram_needed, Instant::now());
        s.entries.push(Entry {
            id,
            name,
            stack: stack.max(MIN_STACK),
            run: Arc::new(run),
        });
        id
    });
    log(
        ESP_LOG_INFO,
        &format!(
            "check {} registered: every {} s (0: off), {} KB of internal RAM",
            name.to_string_lossy(),
            every.as_secs(),
            ram_needed / 1024
        ),
    );
    id
}

/// Change how often a check runs, counted from its last run; zero is off.
/// False if there is no such check.
pub fn set_interval(id: CheckId, every: Duration) -> bool {
    with(|s| s.schedule.set_interval(id, every))
}

/// Hold a check back, as while its module holds a live connection, or let
/// it run again. False if there is no such check.
pub fn set_paused(id: CheckId, paused: bool) -> bool {
    with(|s| s.schedule.set_paused(id, paused))
}

/// When the earliest check falls due, for the power manager's light sleep.
pub(crate) fn next_wake(now: Instant) -> Option<Instant> {
    let wall = wall_s();
    with(|s| s.schedule.next_wake(now, wall))
}

/// A check is due or running: the power manager keeps the CPU up and the
/// network back for it.
pub(crate) fn busy(now: Instant) -> bool {
    let wall = wall_s();
    with(|s| s.schedule.busy(now, wall))
}

// ---------------------------------------------------------------------------
// Running them
// ---------------------------------------------------------------------------

/// The time, in seconds since 1970, if the clock is set.
fn wall_s() -> Option<i64> {
    let us = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_micros() as i64;
    crate::time_policy::plausible(us).then_some(us / 1_000_000)
}

fn internal_free() -> usize {
    #[cfg(not(test))]
    return unsafe { crate::app_manager::get_free_memory() };
    #[cfg(test)]
    usize::MAX
}

fn world() -> World {
    World {
        now: Instant::now(),
        wall_s: wall_s(),
        network: crate::net_manager::net_has_underlay_connection(),
        internal_free: internal_free(),
    }
}

fn skip_words(why: Skip) -> String {
    match why {
        Skip::NoNetwork => "no network".into(),
        Skip::ClockUnset => "the clock is not set".into(),
        Skip::LowRam { free, needed } => format!(
            "{} KB of internal RAM free, {} KB needed",
            free / 1024,
            needed / 1024
        ),
    }
}

fn outcome_words(outcome: &Outcome, wall: Option<i64>) -> String {
    let with = |word: &str, detail: &str| {
        if detail.is_empty() {
            word.to_string()
        } else {
            format!("{word} ({detail})")
        }
    };
    match outcome {
        Outcome::Done(detail) => with("done", detail),
        Outcome::Skipped(why) => with("skipped", why),
        Outcome::Failed(why) => with("failed", why),
        Outcome::StoppedUntil(until) => match wall {
            Some(now) => format!("stopped for {} min", ((until - now).max(0) + 59) / 60),
            None => format!("stopped until {until}"),
        },
    }
}

/// Run a check on its task: held awake, timed, logged, and handed back to
/// the schedule.
fn run_on_task(id: CheckId, name: &'static CStr, stack: usize, run: Run) {
    let _hold = HoldGuard::new(name.to_bytes_with_nul());
    let before = internal_free();
    let started = Instant::now();
    let outcome = run();
    let took = started.elapsed();
    let after = internal_free();
    log(
        ESP_LOG_INFO,
        &format!(
            "check {}: {} in {}.{} s; internal RAM {} KB free before, {} KB after",
            name.to_string_lossy(),
            outcome_words(&outcome, wall_s()),
            took.as_secs(),
            took.subsec_millis() / 100,
            before / 1024,
            after / 1024
        ),
    );
    if let Some(unused) = crate::kernel_task::stack_unused() {
        log(
            ESP_LOG_INFO,
            &format!("check {}: {unused} of {stack} bytes of stack never used", name.to_string_lossy()),
        );
    }
    let w = world();
    with(|s| s.schedule.finished(id, &outcome, &w));
}

/// Start or skip the checks that are due.
fn poll() {
    let w = world();
    let steps = with(|s| {
        s.schedule
            .poll(&w)
            .into_iter()
            .filter_map(|step| {
                let id = match step {
                    Step::Start(id) | Step::Skip(id, _) => id,
                };
                let e = s.entries.iter().find(|e| e.id == id)?;
                Some((step, e.name, e.stack, e.run.clone()))
            })
            .collect::<Vec<_>>()
    });
    for (step, name, stack, run) in steps {
        match step {
            Step::Skip(_, why) => log(
                ESP_LOG_INFO,
                &format!("check {}: skipped: {}", name.to_string_lossy(), skip_words(why)),
            ),
            Step::Start(id) => {
                if !crate::kernel_task::spawn(c"check", stack, crate::kernel_task::PRIORITY_DEFAULT, move || {
                    run_on_task(id, name, stack, run)
                }) {
                    let outcome = Outcome::Failed("no task could be made for it".into());
                    log(
                        ESP_LOG_WARN,
                        &format!("check {}: {}", name.to_string_lossy(), outcome_words(&outcome, None)),
                    );
                    let w = world();
                    with(|s| s.schedule.finished(id, &outcome, &w));
                }
            }
        }
    }
}

unsafe extern "C" fn tick(_arg: *mut c_void) {
    poll();
}

/// Start looking for due checks: a kernel worker job, once a second.
pub(crate) fn start() {
    #[cfg(all(not(target_os = "espidf"), not(test)))]
    sim::register_from_env();
    crate::kworker::every(tick, std::ptr::null_mut(), Duration::from_secs(1));
}

// ---------------------------------------------------------------------------
// The simulator's check
// ---------------------------------------------------------------------------

/// THISTLE_SIM_BACKGROUND_CHECK="SECONDS[:OUTCOME]" registers a check named
/// `sim`, every SECONDS, that says where it runs and ends as OUTCOME says:
/// `done` (the default), `failed`, `skipped`, `stopped` (for an hour), or
/// `lowram` (it asks for more internal RAM than there is, so is never run).
#[cfg(all(not(target_os = "espidf"), not(test)))]
mod sim {
    use super::*;

    pub(super) fn register_from_env() {
        let Ok(spec) = std::env::var("THISTLE_SIM_BACKGROUND_CHECK") else {
            return;
        };
        let mut parts = spec.trim().splitn(2, ':');
        let Some(every) = parts
            .next()
            .and_then(|s| s.trim().parse::<f64>().ok())
            .filter(|s| s.is_finite() && *s >= 0.0)
        else {
            log(ESP_LOG_WARN, &format!("THISTLE_SIM_BACKGROUND_CHECK={spec:?}: no interval"));
            return;
        };
        let outcome = parts.next().unwrap_or("done").trim().to_string();
        let ram_needed = if outcome == "lowram" { usize::MAX / 2 } else { 16 * 1024 };
        register("sim", Duration::from_secs_f64(every), ram_needed, 8 * 1024, move || {
            let task = std::thread::current().name().unwrap_or("?").to_string();
            let held = crate::power_manager::power_manager_held_by();
            let held = if held.is_null() {
                "nothing".to_string()
            } else {
                unsafe { CStr::from_ptr(held) }.to_string_lossy().into_owned()
            };
            // The tasks of that name alive now: this one alone, if the last
            // run's task is gone.
            let alive = std::fs::read_dir("/proc/self/task")
                .map(|tasks| {
                    tasks
                        .flatten()
                        .filter(|t| {
                            std::fs::read_to_string(t.path().join("comm"))
                                .is_ok_and(|comm| comm.trim() == task)
                        })
                        .count()
                })
                .unwrap_or(0);
            log(
                ESP_LOG_INFO,
                &format!("sim check: running on task {task} ({alive} of that name), held awake by {held}"),
            );
            std::thread::sleep(Duration::from_millis(200));
            match outcome.as_str() {
                "failed" => Outcome::Failed("the simulator said so".into()),
                "skipped" => Outcome::Skipped("the simulator said so".into()),
                "stopped" => Outcome::StoppedUntil(wall_s().unwrap_or(0) + 3600),
                _ => Outcome::Done("nothing new".into()),
            }
        });
    }
}

// ---------------------------------------------------------------------------
// FFI — thistle/background_check.h
// ---------------------------------------------------------------------------

/// background_check_result_t
const RESULT_DONE: u32 = 0;
const RESULT_SKIPPED: u32 = 1;
const RESULT_FAILED: u32 = 2;
const RESULT_STOPPED: u32 = 3;

/// background_check_outcome_t
#[repr(C)]
pub struct COutcome {
    pub result: u32,
    /// For BACKGROUND_CHECK_STOPPED: seconds since 1970.
    pub stopped_until: i64,
    /// What it found, or why: NUL-terminated.
    pub detail: [c_char; 96],
}

pub type CCheckFn = unsafe extern "C" fn(arg: *mut c_void, out: *mut COutcome);

impl COutcome {
    fn outcome(&self) -> Outcome {
        let bytes: Vec<u8> = self
            .detail
            .iter()
            .take_while(|&&c| c != 0)
            .map(|&c| c as u8)
            .collect();
        let detail = String::from_utf8_lossy(&bytes).into_owned();
        match self.result {
            RESULT_SKIPPED => Outcome::Skipped(detail),
            RESULT_FAILED => Outcome::Failed(detail),
            RESULT_STOPPED => Outcome::StoppedUntil(self.stopped_until),
            _ => Outcome::Done(detail),
        }
    }
}

/// Register a check from C. Returns its id, or 0 if `name` or `func` is
/// missing.
///
/// # Safety
/// `name` must be a NUL-terminated string; `func(arg, ...)` must be safe to
/// call from another task for as long as the system runs.
#[no_mangle]
pub unsafe extern "C" fn background_check_register(
    name: *const c_char,
    interval_s: u32,
    ram_needed: u32,
    stack: u32,
    func: Option<CCheckFn>,
    arg: *mut c_void,
) -> u32 {
    let (Some(func), false) = (func, name.is_null()) else {
        return 0;
    };
    let name = unsafe { CStr::from_ptr(name) }.to_string_lossy().into_owned();
    let arg = arg as usize;
    register(
        &name,
        Duration::from_secs(interval_s.into()),
        ram_needed as usize,
        stack as usize,
        move || {
            let mut out = COutcome {
                result: RESULT_DONE,
                stopped_until: 0,
                detail: [0; 96],
            };
            unsafe { func(arg as *mut c_void, &mut out) };
            out.outcome()
        },
    )
}

#[no_mangle]
pub extern "C" fn background_check_set_interval(id: u32, interval_s: u32) -> i32 {
    if set_interval(id, Duration::from_secs(interval_s.into())) {
        ESP_OK
    } else {
        ESP_ERR_NOT_FOUND
    }
}

#[no_mangle]
pub extern "C" fn background_check_set_paused(id: u32, paused: bool) -> i32 {
    if set_paused(id, paused) {
        ESP_OK
    } else {
        ESP_ERR_NOT_FOUND
    }
}
