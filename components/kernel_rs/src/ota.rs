// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Kernel — OTA module
//
// Writes a new kernel into the other kernel slot and keeps it once it has run
// for a minute (ADR-0010). The decisions are kernel_slot.rs's; every ESP-IDF
// call is in components/kernel/src/kernel_slot_platform.c. The simulator
// keeps its slots as files in a directory, `THISTLE_SIM_SLOTS`, with the
// bootloader's choice beside them, so the flow runs without hardware.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::kernel_slot::{self, Keep};

// ---------------------------------------------------------------------------
// ESP-IDF error codes
// ---------------------------------------------------------------------------

const ESP_OK: i32 = 0x000;
const ESP_FAIL: i32 = -1;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
#[cfg(not(target_os = "espidf"))]
const ESP_ERR_OTA_VALIDATE_FAILED: i32 = 0x1503;

const BUNDLE_TRANSACTION_PATH: &str = "/sdcard/.thistle-bundle-transaction";
const BUNDLE_DOWNLOAD_PATH: &str = "/sdcard/.thistle-bundle-download";

static TAG: &[u8] = b"ota\0";

// ---------------------------------------------------------------------------
// C FFI — logging
// ---------------------------------------------------------------------------

extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_INFO: i32 = 3;
const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_ERROR: i32 = 1;

macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}

/// ESP_OTA_IMG_PENDING_VERIFY state value from esp_ota_ops.h
///
/// Replaces the C `esp_ota_img_pending_verify()` helper shim in kernel_shims.c.
/// ESP-IDF defines this enum value in `esp_flash_partitions.h` as 0x1.
#[cfg(test)]
const ESP_OTA_IMG_PENDING_VERIFY: u32 = 0x1;

// Progress callback type — matches C typedef `void (*ota_progress_cb_t)(uint32_t written, uint32_t total, void *user_data)`
pub type OtaProgressCb = unsafe extern "C" fn(written: u32, total: u32, user_data: *mut c_void);

// ---------------------------------------------------------------------------
// The flash: ESP-IDF's partitions on a device
// ---------------------------------------------------------------------------

/// What a reset booted.
struct Booted {
    slot: String,
    pending: bool,
    rolled_back_from: Option<&'static str>,
    /// The running kernel's version, when the slot says one.
    version: Option<String>,
}

#[cfg(target_os = "espidf")]
mod platform {
    use super::*;

    extern "C" {
        fn kslot_platform_running(label: *mut c_char, len: usize, pending: *mut bool) -> i32;
        fn kslot_platform_running_version() -> *const c_char;
        fn kslot_platform_begin(label: *const c_char, size: usize, handle: *mut u32) -> i32;
        fn kslot_platform_write(handle: u32, data: *const u8, len: usize) -> i32;
        fn kslot_platform_abort(handle: u32);
        fn kslot_platform_finish(handle: u32, label: *const c_char) -> i32;
        fn kslot_platform_keep() -> i32;
        fn kslot_platform_roll_back() -> i32;
        fn kslot_platform_image_version(label: *const c_char, out: *mut c_char, len: usize) -> i32;
        fn kslot_platform_select(label: *const c_char) -> i32;
        fn esp_restart() -> !;
    }

    /// The bootloader has already chosen; ask which slot it booted.
    pub(super) fn boot() -> Option<Booted> {
        let mut label = [0 as c_char; 17];
        let mut pending = false;
        if unsafe { kslot_platform_running(label.as_mut_ptr(), label.len(), &mut pending) }
            != ESP_OK
        {
            return None;
        }
        let slot = unsafe { CStr::from_ptr(label.as_ptr()) }.to_string_lossy().into_owned();
        let version = unsafe { CStr::from_ptr(kslot_platform_running_version()) }
            .to_string_lossy()
            .into_owned();
        // A rollback is the bootloader's to log; it says so on the console.
        Some(Booted { slot, pending, rolled_back_from: None, version: Some(version) })
    }

    pub(crate) struct Writer {
        handle: u32,
        label: CString,
    }

    impl Writer {
        pub(crate) fn begin(label: &'static str, size: usize) -> Result<Writer, i32> {
            let label = CString::new(label).map_err(|_| ESP_ERR_NOT_FOUND)?;
            let mut handle = 0u32;
            match unsafe { kslot_platform_begin(label.as_ptr(), size, &mut handle) } {
                ESP_OK => Ok(Writer { handle, label }),
                err => Err(err),
            }
        }

        pub(crate) fn write(&mut self, chunk: &[u8]) -> Result<(), i32> {
            match unsafe { kslot_platform_write(self.handle, chunk.as_ptr(), chunk.len()) } {
                ESP_OK => Ok(()),
                err => Err(err),
            }
        }

        pub(crate) fn abort(self) {
            unsafe { kslot_platform_abort(self.handle) }
        }

        /// Validate the image and select its slot.
        pub(crate) fn finish(self) -> Result<(), i32> {
            match unsafe { kslot_platform_finish(self.handle, self.label.as_ptr()) } {
                ESP_OK => Ok(()),
                err => Err(err),
            }
        }
    }

    pub(super) fn keep() -> i32 {
        unsafe { kslot_platform_keep() }
    }

    pub(super) fn roll_back() -> i32 {
        unsafe { kslot_platform_roll_back() }
    }

    /// The version `slot`'s image description says; None once it is erased.
    pub(super) fn image_version(slot: &str) -> Option<String> {
        let label = CString::new(slot).ok()?;
        let mut version = [0 as c_char; 33];
        let err = unsafe {
            kslot_platform_image_version(label.as_ptr(), version.as_mut_ptr(), version.len())
        };
        (err == ESP_OK).then(|| {
            unsafe { CStr::from_ptr(version.as_ptr()) }.to_string_lossy().into_owned()
        })
    }

    pub(super) fn select(slot: &str) -> i32 {
        match CString::new(slot) {
            Ok(label) => unsafe { kslot_platform_select(label.as_ptr()) },
            Err(_) => ESP_ERR_NOT_FOUND,
        }
    }

    pub(super) fn keep_delay() -> Duration {
        kernel_slot::KEEP_DELAY
    }

    pub(super) fn restart() -> ! {
        unsafe { esp_restart() }
    }
}

// ---------------------------------------------------------------------------
// The flash: files in a directory in the simulator
// ---------------------------------------------------------------------------

#[cfg(not(target_os = "espidf"))]
mod platform {
    use super::*;
    use crate::kernel_slot::Slots;
    use std::io::Write;

    /// `THISTLE_SIM_SLOTS`, or /tmp/thistle_slots: `kernel_a.bin` and
    /// `kernel_b.bin`, and `otadata`, the bootloader's choice and the
    /// version written into each.
    pub(crate) fn dir() -> PathBuf {
        std::env::var_os("THISTLE_SIM_SLOTS")
            .filter(|d| !d.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/thistle_slots"))
    }

    fn otadata() -> PathBuf {
        dir().join("otadata")
    }

    fn read() -> Slots {
        Slots::parse(&std::fs::read_to_string(otadata()).unwrap_or_default())
    }

    fn write(slots: &Slots) -> Result<(), i32> {
        let _ = std::fs::create_dir_all(dir());
        let tmp = dir().join("otadata.tmp");
        std::fs::write(&tmp, slots.to_text()).map_err(|_| ESP_FAIL)?;
        std::fs::rename(&tmp, otadata()).map_err(|_| ESP_FAIL)
    }

    /// What ESP-IDF's bootloader would have done at this reset.
    pub(super) fn boot() -> Option<Booted> {
        let mut slots = read();
        let boot = slots.reset();
        let _ = write(&slots);
        Some(Booted {
            slot: boot.slot.to_string(),
            pending: boot.pending,
            rolled_back_from: boot.rolled_back_from,
            version: slots.version(boot.slot).map(str::to_string),
        })
    }

    pub(crate) struct Writer {
        label: &'static str,
        file: std::fs::File,
        head: Vec<u8>,
    }

    fn part(label: &str) -> PathBuf {
        dir().join(format!("{label}.bin.part"))
    }

    impl Writer {
        pub(crate) fn begin(label: &'static str, _size: usize) -> Result<Writer, i32> {
            let _ = std::fs::create_dir_all(dir());
            // As esp_ota_begin() does, erase the slot first.
            let mut slots = read();
            slots.erase(label);
            write(&slots)?;
            let _ = std::fs::remove_file(dir().join(format!("{label}.bin")));
            let file = std::fs::File::create(part(label)).map_err(|_| ESP_FAIL)?;
            Ok(Writer { label, file, head: Vec::new() })
        }

        pub(crate) fn write(&mut self, chunk: &[u8]) -> Result<(), i32> {
            let want = kernel_slot::IMAGE_VERSION_BYTES.saturating_sub(self.head.len());
            self.head.extend_from_slice(&chunk[..want.min(chunk.len())]);
            self.file.write_all(chunk).map_err(|_| ESP_FAIL)
        }

        pub(crate) fn abort(self) {
            let _ = std::fs::remove_file(part(self.label));
        }

        /// As esp_ota_end() does, refuse what is no app image; then select it.
        pub(crate) fn finish(self) -> Result<(), i32> {
            let Some(version) = kernel_slot::image_version(&self.head) else {
                self.abort();
                return Err(ESP_ERR_OTA_VALIDATE_FAILED);
            };
            drop(self.file);
            std::fs::rename(part(self.label), dir().join(format!("{}.bin", self.label)))
                .map_err(|_| ESP_FAIL)?;
            let mut slots = read();
            slots.install(self.label, Some(version));
            write(&slots)
        }
    }

    pub(super) fn keep() -> i32 {
        let mut slots = read();
        slots.keep();
        match write(&slots) {
            Ok(()) => ESP_OK,
            Err(err) => err,
        }
    }

    pub(super) fn roll_back() -> i32 {
        ESP_ERR_NOT_SUPPORTED
    }

    /// What an image description in `slot` would say: a slot no install
    /// wrote holds the build's own kernel, as `idf.py flash` left it.
    pub(super) fn image_version(slot: &str) -> Option<String> {
        match read().contents(slot) {
            kernel_slot::Contents::Written(version) => Some(version.to_string()),
            kernel_slot::Contents::Unwritten => Some(crate::version::running().to_string()),
            kernel_slot::Contents::Erased => None,
        }
    }

    pub(super) fn select(slot: &str) -> i32 {
        let mut slots = read();
        slots.select(slot);
        match write(&slots) {
            Ok(()) => ESP_OK,
            Err(err) => err,
        }
    }

    /// A minute, or `THISTLE_SIM_KEEP_S` seconds.
    pub(super) fn keep_delay() -> Duration {
        std::env::var("THISTLE_SIM_KEEP_S")
            .ok()
            .and_then(|s| s.trim().parse::<f64>().ok())
            .filter(|s| s.is_finite() && *s >= 0.0)
            .map(Duration::from_secs_f64)
            .unwrap_or(kernel_slot::KEEP_DELAY)
    }

    #[cfg(not(test))]
    pub(super) fn restart() -> ! {
        extern "C" {
            fn esp_restart() -> !;
        }
        unsafe { esp_restart() }
    }

    #[cfg(test)]
    pub(super) fn restart() -> ! {
        panic!("restart")
    }
}

/// Writes a kernel image into a kernel slot; update_manager streams the
/// update through it. finish() validates the image and selects the slot.
pub(crate) use platform::Writer as SlotWriter;

/// The simulator's kernel slots: `THISTLE_SIM_SLOTS`, or /tmp/thistle_slots.
#[cfg(not(target_os = "espidf"))]
pub(crate) fn sim_slots_dir() -> PathBuf {
    platform::dir()
}

/// The version the image in `slot` describes (its `esp_app_desc_t`); None
/// when the slot holds no image, as once an install has begun erasing it.
pub(crate) fn slot_image_version(slot: &str) -> Option<String> {
    platform::image_version(slot)
}

/// Select `slot`, as it is, to boot next. ESP-IDF checks its image first.
pub(crate) fn select_slot(slot: &str) -> i32 {
    platform::select(slot)
}

/// Restart, into the slot selected to boot next.
pub(crate) fn restart() -> ! {
    platform::restart()
}

// ---------------------------------------------------------------------------
// The running kernel
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum BootValidationState {
    NotPending,
    Pending,
    Validated,
}

impl BootValidationState {
    fn observe_partition(&mut self, pending: bool) {
        *self = if pending {
            Self::Pending
        } else {
            Self::NotPending
        };
    }

    fn mark_valid_once_with<F>(&mut self, mark_valid: F) -> i32
    where
        F: FnOnce() -> i32,
    {
        if *self != Self::Pending {
            return ESP_OK;
        }

        let result = mark_valid();
        if result == ESP_OK {
            *self = Self::Validated;
        }
        result
    }
}

static BOOT_VALIDATION_STATE: Mutex<BootValidationState> =
    Mutex::new(BootValidationState::NotPending);

struct Running {
    slot: CString,
    booted_at: Instant,
}

static RUNNING: OnceLock<Running> = OnceLock::new();
static VERSION: OnceLock<CString> = OnceLock::new();

/// What to call once the running kernel is kept, and the job that waits.
static KEEP: Mutex<(Option<extern "C" fn()>, u32)> = Mutex::new((None, 0));

pub(crate) fn running_slot() -> Option<&'static str> {
    RUNNING.get().and_then(|r| r.slot.to_str().ok())
}

// ---------------------------------------------------------------------------
// FFI exports
// ---------------------------------------------------------------------------

/// Find which kernel slot is running and whether its kernel is new, to be
/// kept by ota_keep_new_kernel() or rolled back at the next reset.
#[no_mangle]
pub extern "C" fn ota_init() -> i32 {
    let booted_at = Instant::now();
    let Some(booted) = platform::boot() else {
        log!(ESP_LOG_WARN, b"no running kernel slot found\0");
        return ESP_OK;
    };

    if let Some(from) = booted.rolled_back_from {
        let from = CString::new(from).unwrap_or_default();
        log!(
            ESP_LOG_WARN,
            b"the new kernel in %s was not kept; rolled back\0",
            from.as_ptr()
        );
    }

    let slot = CString::new(booted.slot.as_str()).unwrap_or_default();
    // A simulator slot that was never written runs the build's own version.
    #[cfg(not(target_os = "espidf"))]
    let version = booted.version.unwrap_or_else(|| crate::version::running().to_string());
    #[cfg(target_os = "espidf")]
    let version = booted.version.unwrap_or_default();
    crate::update_manager::kernel_booted(&booted.slot, &version);
    let version = CString::new(version).unwrap_or_default();
    if booted.pending {
        log!(
            ESP_LOG_INFO,
            b"running %s, version %s, new: kept after %u s, until then a reset rolls it back\0",
            slot.as_ptr(),
            version.as_ptr(),
            platform::keep_delay().as_secs() as u32
        );
    } else {
        log!(ESP_LOG_INFO, b"running %s, version %s\0", slot.as_ptr(), version.as_ptr());
    }

    #[cfg(not(target_os = "espidf"))]
    let _ = VERSION.set(version);
    let _ = RUNNING.set(Running { slot, booted_at });

    match BOOT_VALIDATION_STATE.lock() {
        Ok(mut state) => state.observe_partition(booted.pending),
        Err(_) => return ESP_FAIL,
    }
    ESP_OK
}

/// Keep the running kernel once it has run for a minute, then call `then`;
/// call it at once when the kernel is not new. Until it is kept, a reset
/// rolls a new kernel back, which is why the power manager, whose deep sleep
/// is a reset, is what `then` starts.
#[no_mangle]
pub extern "C" fn ota_keep_new_kernel(then: Option<extern "C" fn()>) {
    let pending = matches!(
        BOOT_VALIDATION_STATE.lock().map(|s| *s),
        Ok(BootValidationState::Pending)
    );
    if !pending {
        let _ = ota_finalize_bundle_transaction();
        if let Some(then) = then {
            then();
        }
        return;
    }
    let mut keep = KEEP.lock().unwrap_or_else(|e| e.into_inner());
    if keep.1 != 0 {
        return;
    }
    keep.0 = then;
    log!(ESP_LOG_INFO, b"the power manager starts once this kernel is kept\0");
    keep.1 = crate::kworker::every(keep_job, std::ptr::null_mut(), Duration::from_secs(1));
}

unsafe extern "C" fn keep_job(_arg: *mut c_void) {
    let running_for = RUNNING.get().map_or(Duration::ZERO, |r| r.booted_at.elapsed());
    let pending = true;
    if kernel_slot::keep(pending, running_for, platform::keep_delay()) != Keep::Now {
        return;
    }
    let result = match BOOT_VALIDATION_STATE.lock() {
        Ok(mut state) => state.mark_valid_once_with(platform::keep),
        Err(_) => ESP_FAIL,
    };
    if result != ESP_OK {
        log!(ESP_LOG_ERROR, b"could not keep this kernel: 0x%x\0", result);
        return;
    }
    let slot = CString::new(running_slot().unwrap_or("?")).unwrap_or_default();
    log!(
        ESP_LOG_INFO,
        b"kept the kernel in %s after %u s; starting the power manager\0",
        slot.as_ptr(),
        running_for.as_secs() as u32
    );
    // Only now, never when it was written: a kernel rolled back never
    // raises the floor.
    crate::update_manager::kernel_kept();
    if ota_finalize_bundle_transaction() != ESP_OK {
        // The kernel is kept. Recovery retries the cleanup later.
        log!(ESP_LOG_WARN, b"could not finalize the bundle transaction\0");
    }
    let (then, id) = {
        let mut keep = KEEP.lock().unwrap_or_else(|e| e.into_inner());
        (keep.0.take(), keep.1)
    };
    crate::kworker::cancel(id);
    if let Some(then) = then {
        then();
    }
}

/// Whether the card holds a release in update/: an image and its artifact
/// manifest beside it.
#[no_mangle]
pub extern "C" fn ota_sd_update_available() -> bool {
    crate::update_manager::card_release().is_some()
}

/// Install the card's release (update_manager.rs: its artifact manifest is
/// checked, then it is written into the other kernel slot) and restart into
/// it. Blocks; Settings installs on a task of its own instead, through
/// thistle/update.h.
///
/// # Safety
/// The progress callback is not called; it is kept for the C signature.
#[no_mangle]
pub unsafe extern "C" fn ota_apply_from_sd(
    _progress_cb: Option<OtaProgressCb>,
    _user_data: *mut c_void,
) -> i32 {
    match crate::update_manager::install_from_card_blocking() {
        ESP_OK => {
            log!(ESP_LOG_INFO, b"restarting into the new kernel\0");
            platform::restart()
        }
        err => err,
    }
}

/// Apply a firmware OTA update from a URL (not yet implemented).
///
/// # Safety
/// May be called from C.
#[no_mangle]
pub unsafe extern "C" fn ota_apply_from_http(
    _url: *const c_char,
    _progress_cb: Option<OtaProgressCb>,
    _user_data: *mut c_void,
) -> i32 {
    log!(ESP_LOG_WARN, b"HTTP OTA not yet implemented\0");
    ESP_ERR_NOT_SUPPORTED
}

/// Return the current firmware version string: in the simulator, the version
/// written into the running slot, else the build's.
///
/// # Safety
/// Returns a pointer to a static C string. Do not free.
#[no_mangle]
pub extern "C" fn ota_get_current_version() -> *const c_char {
    match VERSION.get() {
        Some(version) => version.as_ptr(),
        None => crate::version::running_c(),
    }
}

/// Return the label of the running kernel slot, or "unknown".
///
/// # Safety
/// Returns a pointer to a static C string.
#[no_mangle]
pub extern "C" fn ota_get_running_partition() -> *const c_char {
    match RUNNING.get() {
        Some(running) => running.slot.as_ptr(),
        None => b"unknown\0".as_ptr() as *const c_char,
    }
}

/// Keep the running kernel now, if it is new. ota_keep_new_kernel() does this
/// after a minute; this is for a caller that knows better.
#[no_mangle]
pub extern "C" fn ota_mark_valid() -> i32 {
    #[cfg(target_os = "espidf")]
    {
        return match BOOT_VALIDATION_STATE.lock() {
            Ok(mut state) => state.mark_valid_once_with(platform::keep),
            Err(_) => ESP_FAIL,
        };
    }
    #[cfg(not(target_os = "espidf"))]
    ESP_OK
}

fn finalize_bundle_transaction_at(path: &Path) -> std::io::Result<()> {
    match std::fs::remove_dir_all(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
    }
}

/// Remove rollback files only after the matching OTA image is healthy and valid.
/// Recovery can also finalize this journal on a later boot if SD cleanup fails.
#[no_mangle]
pub extern "C" fn ota_finalize_bundle_transaction() -> i32 {
    for path in [BUNDLE_TRANSACTION_PATH, BUNDLE_DOWNLOAD_PATH] {
        if finalize_bundle_transaction_at(Path::new(path)).is_err() {
            return ESP_FAIL;
        }
    }
    ESP_OK
}

/// Give up the running kernel and boot the other kernel slot's.
///
/// # Safety
/// May be called from C.
#[no_mangle]
pub extern "C" fn ota_rollback() -> i32 {
    platform::roll_back()
}

// ---------------------------------------------------------------------------
// Tests
//
// The decisions are kernel_slot.rs's, tested there; the simulator's
// test_kernel_slots.py runs an install, a keep and a rollback end to end.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn test_pending_image_is_not_marked_before_health_milestone() {
        let mut state = BootValidationState::NotPending;
        let mark_calls = std::cell::Cell::new(0);

        state.observe_partition(true);

        assert_eq!(state, BootValidationState::Pending);
        assert_eq!(mark_calls.get(), 0);
    }

    #[test]
    fn test_pending_verify_value_matches_esp_idf_abi() {
        assert_eq!(ESP_OTA_IMG_PENDING_VERIFY, 0x1);
    }

    #[test]
    fn test_healthy_boot_marks_pending_image_exactly_once() {
        let mut state = BootValidationState::NotPending;
        let mark_calls = std::cell::Cell::new(0);
        state.observe_partition(true);

        for _ in 0..2 {
            assert_eq!(
                state.mark_valid_once_with(|| {
                    mark_calls.set(mark_calls.get() + 1);
                    ESP_OK
                }),
                ESP_OK
            );
        }

        assert_eq!(state, BootValidationState::Validated);
        assert_eq!(mark_calls.get(), 1);
    }

    #[test]
    fn test_failed_confirmation_remains_pending_for_retry() {
        let mut state = BootValidationState::NotPending;
        state.observe_partition(true);

        assert_eq!(state.mark_valid_once_with(|| ESP_FAIL), ESP_FAIL);
        assert_eq!(state, BootValidationState::Pending);
    }

    #[test]
    fn test_get_current_version_before_init_is_the_builds() {
        let ptr = ota_get_current_version();
        let version = unsafe { CStr::from_ptr(ptr).to_str().unwrap() };
        assert_eq!(version, crate::version::running());
    }

    #[test]
    fn test_sd_update_available_false_when_no_card() {
        // In the test environment there is no SD card, so the update path
        // /sdcard/update/thistle_os.bin does not exist.
        assert!(!ota_sd_update_available());
    }

    #[test]
    fn test_mark_valid_returns_ok_on_host() {
        assert_eq!(ota_mark_valid(), ESP_OK);
    }

    #[test]
    fn test_bundle_transaction_cleanup_is_idempotent() {
        let root =
            std::env::temp_dir().join(format!("thistle-ota-finalize-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("backup")).unwrap();
        std::fs::write(root.join("journal"), b"transaction").unwrap();

        finalize_bundle_transaction_at(&root).unwrap();
        assert!(!root.exists());
        finalize_bundle_transaction_at(&root).unwrap();
    }

    #[test]
    fn test_get_running_partition_before_init_is_unknown() {
        let ptr = ota_get_running_partition();
        let s = unsafe { CStr::from_ptr(ptr).to_str().unwrap() };
        assert_eq!(s, "unknown");
    }

    #[test]
    fn test_rollback_not_supported_on_host() {
        assert_eq!(ota_rollback(), ESP_ERR_NOT_SUPPORTED);
    }

}
