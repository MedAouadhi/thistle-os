// SPDX-License-Identifier: BSD-3-Clause
//! The kernel worker: one thread that runs short jobs, one after another, for
//! the parts of the system that would otherwise each keep a thread of their
//! own.
//!
//! Every thread costs a stack, and stacks belong in internal RAM: one in PSRAM
//! cannot be read while flash is written, so its thread must never touch
//! flash, which is easy to break without noticing. Internal RAM is also what
//! runs out. MeshCore's loop and the GPS reader each kept a thread, with
//! 20 KB and 4 KB of stack, for loops that wait almost all of the time. Here
//! they share one 16 KB stack: jobs run one at a time, so the stack has to
//! fit the deepest job rather than all of them together.
//!
//! A job must return promptly, since nothing else runs here meanwhile. The
//! longest are MeshCore's LoRa sends, which return once the packet is on the
//! air, a second or two at most; the GPS reader's UART buffer holds more than
//! that. Anything that waits on a network or a modem has a thread elsewhere
//! (net_coordinator).

use std::os::raw::c_void;
use std::sync::{Condvar, Mutex, MutexGuard, OnceLock, PoisonError};
use std::thread::ThreadId;
use std::time::{Duration, Instant};

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_FOUND: i32 = 0x105;

/// The deepest job measured is MeshCore's crypto, at 10.7 KB.
const STACK_SIZE: usize = 16 * 1024;

/// Warn once if a job leaves less stack than this unused.
#[cfg(target_os = "espidf")]
const STACK_LOW_WATER: u32 = 4 * 1024;

/// A job: called with the argument it was posted with.
pub type JobFn = unsafe extern "C" fn(arg: *mut c_void);

struct Job {
    id: u32,
    func: JobFn,
    /// The caller's pointer, carried as an integer so the queue is Send.
    arg: usize,
    /// None for a job that runs once.
    period: Option<Duration>,
    due: Instant,
}

struct Queue {
    jobs: Vec<Job>,
    next_id: u32,
    /// The job running now.
    running: Option<u32>,
}

static QUEUE: Mutex<Queue> = Mutex::new(Queue {
    jobs: Vec::new(),
    next_id: 1,
    running: None,
});
/// A job was added or removed.
static CHANGED: Condvar = Condvar::new();
/// A job finished running.
static FINISHED: Condvar = Condvar::new();
static WORKER: OnceLock<ThreadId> = OnceLock::new();

fn lock() -> MutexGuard<'static, Queue> {
    QUEUE.lock().unwrap_or_else(PoisonError::into_inner)
}

fn on_worker() -> bool {
    WORKER.get() == Some(&std::thread::current().id())
}

#[cfg(target_os = "espidf")]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
    fn uxTaskGetStackHighWaterMark(task: *mut c_void) -> u32;
    // components/kernel/src/link_deps.c
    fn thistle_pthread_name_next(name: *const u8) -> i32;
}

/// Add a job, due `delay` from now. Returns its id.
fn add(func: JobFn, arg: *mut c_void, delay: Duration, period: Option<Duration>) -> u32 {
    let mut q = lock();
    let id = q.next_id;
    q.next_id = q.next_id.wrapping_add(1).max(1);
    q.jobs.push(Job {
        id,
        func,
        arg: arg as usize,
        period,
        due: Instant::now() + delay,
    });
    drop(q);
    CHANGED.notify_all();
    id
}

/// Run `func(arg)` once on the worker, as soon as it is free.
pub fn post(func: JobFn, arg: *mut c_void) -> u32 {
    add(func, arg, Duration::ZERO, None)
}

/// Run `func(arg)` once on the worker, `delay` from now or once it is free
/// after that.
pub fn after(func: JobFn, arg: *mut c_void, delay: Duration) -> u32 {
    add(func, arg, delay, None)
}

/// Run `func(arg)` on the worker now, and then `period` after each run ends,
/// until cancelled.
pub fn every(func: JobFn, arg: *mut c_void, period: Duration) -> u32 {
    add(func, arg, Duration::ZERO, Some(period))
}

/// Stop a job. Once this returns, the job is not running and will not run
/// again, unless this is called from a job, which may be the one cancelled:
/// it then just runs no more. False if there is no such job.
pub fn cancel(id: u32) -> bool {
    let mut q = lock();
    let before = q.jobs.len();
    q.jobs.retain(|j| j.id != id);
    let found = q.jobs.len() != before || q.running == Some(id);
    if !on_worker() {
        while q.running == Some(id) {
            q = FINISHED.wait(q).unwrap_or_else(PoisonError::into_inner);
        }
    }
    drop(q);
    CHANGED.notify_all();
    found
}

fn run() {
    let mut q = lock();
    let mut warned = false;
    loop {
        let now = Instant::now();
        // Earliest first; among equals, first posted.
        let next = q
            .jobs
            .iter()
            .enumerate()
            .min_by_key(|(_, j)| (j.due, j.id))
            .map(|(i, j)| (i, j.due));
        let i = match next {
            Some((i, due)) if due <= now => i,
            Some((_, due)) => {
                q = CHANGED
                    .wait_timeout(q, due - now)
                    .unwrap_or_else(PoisonError::into_inner)
                    .0;
                continue;
            }
            None => {
                q = CHANGED.wait(q).unwrap_or_else(PoisonError::into_inner);
                continue;
            }
        };

        let (id, func, arg, periodic) = {
            let j = &q.jobs[i];
            (j.id, j.func, j.arg, j.period.is_some())
        };
        if periodic {
            // Kept in the queue, so cancel() finds it, but not due while it
            // runs.
            q.jobs[i].due = now + Duration::from_secs(3600 * 24 * 365);
        } else {
            q.jobs.swap_remove(i);
        }
        q.running = Some(id);
        drop(q);

        unsafe { func(arg as *mut c_void) };
        check_stack(&mut warned);

        q = lock();
        q.running = None;
        let ended = Instant::now();
        if let Some(j) = q.jobs.iter_mut().find(|j| j.id == id) {
            if let Some(period) = j.period {
                j.due = ended + period;
            }
        }
        FINISHED.notify_all();
    }
}

#[cfg(target_os = "espidf")]
fn check_stack(warned: &mut bool) {
    if *warned {
        return;
    }
    let left = unsafe { uxTaskGetStackHighWaterMark(std::ptr::null_mut()) };
    if left < STACK_LOW_WATER {
        *warned = true;
        unsafe {
            esp_log_write(
                2, /* WARN */
                b"kworker\0".as_ptr(),
                b"kworker: a job left %u of %u bytes of stack unused\n\0".as_ptr(),
                left,
                STACK_SIZE as u32,
            )
        };
    }
}

#[cfg(not(target_os = "espidf"))]
fn check_stack(_warned: &mut bool) {}

/// Start the worker. Jobs added before this wait for it.
pub fn start() -> bool {
    static STARTED: Mutex<bool> = Mutex::new(false);
    let mut started = STARTED.lock().unwrap_or_else(PoisonError::into_inner);
    if *started {
        return true;
    }
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(b"kworker\0".as_ptr())
    };
    let spawned = std::thread::Builder::new()
        .name("kworker".to_string())
        .stack_size(STACK_SIZE)
        .spawn(|| {
            let _ = WORKER.set(std::thread::current().id());
            run()
        });
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(std::ptr::null())
    };
    *started = spawned.is_ok();
    *started
}

// ---------------------------------------------------------------------------
// FFI — thistle/kworker.h
// ---------------------------------------------------------------------------

/// Start the worker; kernel_init() does this before any driver starts.
#[no_mangle]
pub extern "C" fn kworker_start() -> i32 {
    if start() {
        ESP_OK
    } else {
        ESP_FAIL
    }
}

/// Run `func(arg)` once on the worker. Returns the job's id, or 0 for a NULL
/// `func`.
#[no_mangle]
pub extern "C" fn kworker_post(func: Option<JobFn>, arg: *mut c_void) -> u32 {
    func.map_or(0, |f| post(f, arg))
}

/// Run `func(arg)` on the worker now and then `period_ms` after each run ends.
/// Returns the job's id, or 0 for a NULL `func` or a zero period.
#[no_mangle]
pub extern "C" fn kworker_every(func: Option<JobFn>, arg: *mut c_void, period_ms: u32) -> u32 {
    match func {
        Some(f) if period_ms > 0 => every(f, arg, Duration::from_millis(period_ms as u64)),
        _ => 0,
    }
}

/// Stop a job; see `cancel`.
#[no_mangle]
pub extern "C" fn kworker_cancel(id: u32) -> i32 {
    if id == 0 {
        ESP_ERR_INVALID_ARG
    } else if cancel(id) {
        ESP_OK
    } else {
        ESP_ERR_NOT_FOUND
    }
}

/// Whether the caller is a job.
#[no_mangle]
pub extern "C" fn kworker_is_current() -> bool {
    on_worker()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
    use std::sync::Arc;

    fn wait_until(what: impl Fn() -> bool) -> bool {
        let until = Instant::now() + Duration::from_secs(5);
        while Instant::now() < until {
            if what() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        false
    }

    unsafe extern "C" fn count(arg: *mut c_void) {
        (*(arg as *const AtomicU32)).fetch_add(1, Ordering::SeqCst);
    }

    #[test]
    fn a_posted_job_runs_once() {
        assert!(start());
        let n: &'static AtomicU32 = Box::leak(Box::new(AtomicU32::new(0)));
        post(count, n as *const _ as *mut c_void);
        assert!(wait_until(|| n.load(Ordering::SeqCst) == 1));
        std::thread::sleep(Duration::from_millis(30));
        assert_eq!(n.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn a_periodic_job_repeats_until_cancelled_and_not_after() {
        assert!(start());
        let n: &'static AtomicU32 = Box::leak(Box::new(AtomicU32::new(0)));
        let id = every(count, n as *const _ as *mut c_void, Duration::from_millis(5));
        assert!(wait_until(|| n.load(Ordering::SeqCst) >= 3));
        assert!(cancel(id));
        let after = n.load(Ordering::SeqCst);
        std::thread::sleep(Duration::from_millis(40));
        assert_eq!(n.load(Ordering::SeqCst), after);
        assert!(!cancel(id), "a job is cancelled once");
    }

    static ORDER: Mutex<Vec<usize>> = Mutex::new(Vec::new());
    unsafe extern "C" fn record(arg: *mut c_void) {
        ORDER.lock().unwrap().push(arg as usize);
    }
    unsafe extern "C" fn hold(arg: *mut c_void) {
        let gate = &*(arg as *const (Mutex<bool>, Condvar));
        let mut open = gate.0.lock().unwrap();
        while !*open {
            open = gate.1.wait(open).unwrap();
        }
    }

    #[test]
    fn jobs_run_one_at_a_time_in_the_order_posted() {
        assert!(start());
        let gate: &'static (Mutex<bool>, Condvar) =
            Box::leak(Box::new((Mutex::new(false), Condvar::new())));
        ORDER.lock().unwrap().clear();
        // While the first holds the worker, the rest queue behind it.
        post(hold, gate as *const _ as *mut c_void);
        for i in 1..=5usize {
            post(record, i as *mut c_void);
        }
        std::thread::sleep(Duration::from_millis(20));
        assert!(ORDER.lock().unwrap().is_empty());
        *gate.0.lock().unwrap() = true;
        gate.1.notify_all();
        assert!(wait_until(|| ORDER.lock().unwrap().len() == 5));
        assert_eq!(*ORDER.lock().unwrap(), vec![1, 2, 3, 4, 5]);
    }

    static BUSY: AtomicUsize = AtomicUsize::new(0);
    static BUSY_RUNS: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn busy(_: *mut c_void) {
        BUSY.store(1, Ordering::SeqCst);
        std::thread::sleep(Duration::from_millis(20));
        BUSY_RUNS.fetch_add(1, Ordering::SeqCst);
        BUSY.store(0, Ordering::SeqCst);
    }

    #[test]
    fn cancel_waits_for_a_running_job_to_finish() {
        assert!(start());
        BUSY_RUNS.store(0, Ordering::SeqCst);
        let id = every(busy, std::ptr::null_mut(), Duration::from_millis(1));
        assert!(wait_until(|| BUSY.load(Ordering::SeqCst) == 1));
        assert!(cancel(id));
        assert_eq!(BUSY.load(Ordering::SeqCst), 0, "returned while it ran");
        let runs = BUSY_RUNS.load(Ordering::SeqCst);
        std::thread::sleep(Duration::from_millis(50));
        assert_eq!(BUSY_RUNS.load(Ordering::SeqCst), runs);
    }

    static SELF_ID: AtomicU32 = AtomicU32::new(0);
    static SELF_RUNS: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn cancels_itself(_: *mut c_void) {
        assert!(kworker_is_current());
        if SELF_RUNS.fetch_add(1, Ordering::SeqCst) + 1 == 2 {
            // Would wait for itself for ever if cancel() waited here.
            assert!(cancel(SELF_ID.load(Ordering::SeqCst)));
        }
    }

    #[test]
    fn a_job_can_cancel_itself() {
        assert!(start());
        SELF_RUNS.store(0, Ordering::SeqCst);
        let id = every(cancels_itself, std::ptr::null_mut(), Duration::from_millis(1));
        SELF_ID.store(id, Ordering::SeqCst);
        assert!(wait_until(|| SELF_RUNS.load(Ordering::SeqCst) >= 2));
        std::thread::sleep(Duration::from_millis(30));
        assert_eq!(SELF_RUNS.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn the_c_interface_refuses_what_it_cannot_run() {
        assert_eq!(kworker_post(None, std::ptr::null_mut()), 0);
        assert_eq!(kworker_every(Some(count), std::ptr::null_mut(), 0), 0);
        assert_eq!(kworker_cancel(0), ESP_ERR_INVALID_ARG);
        assert_eq!(kworker_cancel(u32::MAX - 7), ESP_ERR_NOT_FOUND);
        assert!(!kworker_is_current());
    }
}
