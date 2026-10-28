// SPDX-License-Identifier: BSD-3-Clause
//! One-shot tasks: run a job on a FreeRTOS task of its own, gone when the
//! job is done (update_manager, tailnet_manager, gps_assist).
//!
//! Not a std::thread: on ESP-IDF one leaves its `Thread` handle behind when
//! it ends, about 150 bytes of internal RAM, which a heap trace caught at
//! two threads a restart. The task's stack is internal RAM, as any task's
//! that writes flash, NVS or SPIFFS must be, and the job's box is freed
//! before the task deletes itself, so nothing is left behind. In the
//! simulator the job runs on a std::thread.

#[cfg(target_os = "espidf")]
use std::os::raw::{c_char, c_void};

/// The FreeRTOS priority of a pthread (CONFIG_PTHREAD_TASK_PRIO_DEFAULT).
pub const PRIORITY_DEFAULT: u32 = 5;

/// Run `job` on a task named `name` with `stack` bytes of stack at
/// `priority`. False if the task could not be made; the job is then
/// dropped unrun.
pub fn spawn<F>(name: &'static std::ffi::CStr, stack: usize, priority: u32, job: F) -> bool
where
    F: FnOnce() + Send + 'static,
{
    platform::spawn(name, stack, priority, Box::new(job))
}

/// How many bytes of the calling task's stack have never been used, on a
/// device; None in the simulator.
pub fn stack_unused() -> Option<u32> {
    platform::stack_unused()
}

type Job = Box<dyn FnOnce() + Send>;

#[cfg(target_os = "espidf")]
mod platform {
    use super::*;

    extern "C" {
        fn xTaskCreatePinnedToCore(
            task_fn: unsafe extern "C" fn(*mut c_void),
            name: *const c_char,
            stack_depth: u32,
            param: *mut c_void,
            priority: u32,
            task_handle: *mut *mut c_void,
            core_id: i32,
        ) -> i32;
        fn vTaskDelete(task: *mut c_void);
        fn uxTaskGetStackHighWaterMark(task: *mut c_void) -> u32;
    }
    const PD_PASS: i32 = 1;
    const NO_AFFINITY: i32 = 0x7fff_ffff;

    unsafe extern "C" fn entry(arg: *mut c_void) {
        {
            // Dropped here: nothing after vTaskDelete() runs.
            let job = unsafe { Box::from_raw(arg as *mut Job) };
            (*job)();
        }
        unsafe { vTaskDelete(std::ptr::null_mut()) };
    }

    pub(super) fn spawn(name: &'static std::ffi::CStr, stack: usize, priority: u32, job: Job) -> bool {
        let arg = Box::into_raw(Box::new(job)) as *mut c_void;
        let created = unsafe {
            xTaskCreatePinnedToCore(entry, name.as_ptr(), stack as u32, arg, priority,
                                    std::ptr::null_mut(), NO_AFFINITY)
        } == PD_PASS;
        if !created {
            drop(unsafe { Box::from_raw(arg as *mut Job) });
        }
        created
    }

    pub(super) fn stack_unused() -> Option<u32> {
        Some(unsafe { uxTaskGetStackHighWaterMark(std::ptr::null_mut()) })
    }
}

#[cfg(not(target_os = "espidf"))]
mod platform {
    use super::*;

    pub(super) fn spawn(name: &'static std::ffi::CStr, stack: usize, _priority: u32, job: Job) -> bool {
        std::thread::Builder::new()
            .name(name.to_string_lossy().into_owned())
            .stack_size(stack)
            .spawn(job)
            .is_ok()
    }

    pub(super) fn stack_unused() -> Option<u32> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    #[test]
    fn a_job_runs_once_on_a_task_of_its_own() {
        let (tx, rx) = mpsc::channel();
        assert!(spawn(c"test", 16 * 1024, PRIORITY_DEFAULT, move || {
            tx.send(std::thread::current().name().map(str::to_string)).unwrap();
        }));
        assert_eq!(rx.recv().unwrap().as_deref(), Some("test"));
    }
}
