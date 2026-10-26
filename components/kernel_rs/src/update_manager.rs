// SPDX-License-Identifier: BSD-3-Clause
//! System updates: runs what `update_policy` decides (ADR-0010,
//! `.scratch/ota/spec.md`, "The update manager").
//!
//! For now, the card install: a release copied to `/sdcard/update/`, its
//! `.bin`, the artifact manifest beside it (`<bin>.manifest`) and the
//! manifest's signature (`<bin>.manifest.sig`), as the release assets are
//! named. The manifest's signature is checked against the kernel's key, the
//! manifest against the device (`update_policy::accept`), the file's size
//! against the manifest's; then the file is streamed into the other kernel
//! slot, hashed as it goes, and the slot is selected only once its SHA-256
//! is the manifest's and ESP-IDF has validated the image. Settings then
//! offers Restart now or Later; Later is the next restart of any kind.
//!
//! It also keeps the security floor and each kernel slot's record (its
//! version and security version) in NVS, `thistle_sec`, Recovery's
//! namespace: a slot's record is written with the slot, and by a kernel for
//! its own slot when it runs; the floor rises to a kernel's security
//! version when that kernel is kept (ota.rs), so one rolled back never
//! raises it. Every NVS call is in `components/kernel/src/update_platform.c`;
//! the simulator keeps the same in `nvs`, a file beside its kernel slots.
//!
//! An install runs on a FreeRTOS task of its own with a 16 KB internal
//! stack, gone when it is done, and holds the device awake meanwhile.
//! The C API is `thistle/update.h`.

use std::ffi::{CStr, CString};
use std::io::Read;
use std::os::raw::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Mutex;

use sha2::{Digest, Sha256};
use thistle_artifact::ArtifactManifest;

use crate::update_policy::{self as policy, Device, Refusal, Source};

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;
const ESP_ERR_INVALID_CRC: i32 = 0x109;

/// A manifest is eleven short lines; Recovery bounds it the same.
const MAX_MANIFEST: u64 = 4096;
/// The install's task: an Ed25519 verify is 7 to 8 KB deep, as the App
/// Store worker found. Internal RAM, as it writes flash.
const STACK_SIZE: usize = 16 * 1024;
/// Copied a chunk at a time: at most 4 KB stays in internal RAM.
const CHUNK: usize = 4096;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"update\0";

#[cfg(not(test))]
fn log(level: i32, text: &str) {
    let text = CString::new(text.replace('\0', " ")).unwrap_or_default();
    unsafe { esp_log_write(level, TAG.as_ptr(), b"%s\0".as_ptr(), text.as_ptr()) }
}
#[cfg(test)]
fn log(_level: i32, _text: &str) {}

const ESP_LOG_ERROR: i32 = 1;
const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

// ---------------------------------------------------------------------------
// The state, as Settings reads it
// ---------------------------------------------------------------------------

/// update_state_t
pub const STATE_IDLE: u32 = 0;
pub const STATE_CHECKING: u32 = 1;
pub const STATE_UP_TO_DATE: u32 = 2;
pub const STATE_AVAILABLE: u32 = 3;
pub const STATE_DOWNLOADING: u32 = 4;
pub const STATE_WRITING: u32 = 5;
pub const STATE_READY: u32 = 6;
pub const STATE_FAILED: u32 = 7;

struct Status {
    state: u32,
    done: u32,
    total: u32,
    /// The version being installed, or installed and waiting for a restart.
    version: String,
    /// Why the last install failed.
    reason: String,
}

static STATUS: Mutex<Status> = Mutex::new(Status {
    state: STATE_IDLE,
    done: 0,
    total: 0,
    version: String::new(),
    reason: String::new(),
});

/// An install is under way; only one at a time.
static BUSY: AtomicBool = AtomicBool::new(false);

fn with<R>(f: impl FnOnce(&mut Status) -> R) -> R {
    let mut status = STATUS.lock().unwrap_or_else(|e| e.into_inner());
    f(&mut status)
}

/// The running kernel's security version: the build's on a device; in the
/// simulator, the one recorded for the slot it "booted".
static RUNNING_SECURITY: AtomicU32 = AtomicU32::new(crate::version::SECURITY_VERSION);

// ---------------------------------------------------------------------------
// NVS: the floor and the slots' records
// ---------------------------------------------------------------------------

/// What NVS holds of a kernel slot.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Record {
    pub version: String,
    pub security_version: u32,
}

#[cfg(target_os = "espidf")]
mod platform {
    use super::*;

    extern "C" {
        fn update_platform_floor(floor: *mut u32) -> i32;
        fn update_platform_set_floor(floor: u32) -> i32;
        fn update_platform_record(slot: *const c_char, version: *const c_char, sec: u32) -> i32;
        fn update_platform_get_record(
            slot: *const c_char,
            version: *mut c_char,
            len: usize,
            sec: *mut u32,
        ) -> i32;
    }

    pub(super) fn floor() -> Result<u32, i32> {
        let mut floor = 0u32;
        match unsafe { update_platform_floor(&mut floor) } {
            ESP_OK => Ok(floor),
            err => Err(err),
        }
    }

    pub(super) fn set_floor(floor: u32) -> i32 {
        unsafe { update_platform_set_floor(floor) }
    }

    pub(super) fn record(slot: &str, record: &Record) -> i32 {
        let (Ok(slot), Ok(version)) = (CString::new(slot), CString::new(record.version.as_str()))
        else {
            return ESP_FAIL;
        };
        unsafe { update_platform_record(slot.as_ptr(), version.as_ptr(), record.security_version) }
    }

    pub(super) fn get_record(slot: &str) -> Option<Record> {
        let slot = CString::new(slot).ok()?;
        let mut version = [0 as c_char; 40];
        let mut sec = 0u32;
        let err = unsafe {
            update_platform_get_record(slot.as_ptr(), version.as_mut_ptr(), version.len(), &mut sec)
        };
        (err == ESP_OK).then(|| Record {
            version: unsafe { CStr::from_ptr(version.as_ptr()) }.to_string_lossy().into_owned(),
            security_version: sec,
        })
    }
}

/// The simulator's NVS: `nvs`, `key=value` lines beside its kernel slots,
/// with the keys update_platform.c uses.
#[cfg(not(target_os = "espidf"))]
mod platform {
    use super::*;

    fn path() -> PathBuf {
        crate::ota::sim_slots_dir().join("nvs")
    }

    fn read() -> Vec<(String, String)> {
        parse_nvs(&std::fs::read_to_string(path()).unwrap_or_default())
    }

    fn write(entries: &[(String, String)]) -> i32 {
        let _ = std::fs::create_dir_all(crate::ota::sim_slots_dir());
        let tmp = path().with_extension("tmp");
        let text: String = entries.iter().map(|(k, v)| format!("{k}={v}\n")).collect();
        match std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, path())) {
            Ok(()) => ESP_OK,
            Err(_) => ESP_FAIL,
        }
    }

    fn get(entries: &[(String, String)], key: &str) -> Option<String> {
        entries.iter().find(|(k, _)| k == key).map(|(_, v)| v.clone())
    }

    fn set(entries: &mut Vec<(String, String)>, key: &str, value: String) {
        match entries.iter_mut().find(|(k, _)| k == key) {
            Some(entry) => entry.1 = value,
            None => entries.push((key.to_string(), value)),
        }
    }

    pub(super) fn floor() -> Result<u32, i32> {
        match get(&read(), "fw_sec_ver") {
            None => Ok(0),
            Some(v) => v.trim().parse().map_err(|_| ESP_FAIL),
        }
    }

    pub(super) fn set_floor(floor: u32) -> i32 {
        let mut entries = read();
        set(&mut entries, "fw_sec_ver", floor.to_string());
        write(&entries)
    }

    pub(super) fn record(slot: &str, record: &Record) -> i32 {
        let mut entries = read();
        set(&mut entries, &format!("{slot}_ver"), record.version.clone());
        set(&mut entries, &format!("{slot}_sec"), record.security_version.to_string());
        write(&entries)
    }

    pub(super) fn get_record(slot: &str) -> Option<Record> {
        let entries = read();
        Some(Record {
            version: get(&entries, &format!("{slot}_ver"))?,
            security_version: get(&entries, &format!("{slot}_sec"))?.trim().parse().ok()?,
        })
    }
}

/// `key=value` lines; anything else is skipped.
#[cfg(not(target_os = "espidf"))]
fn parse_nvs(text: &str) -> Vec<(String, String)> {
    text.lines()
        .filter_map(|line| line.split_once('='))
        .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
        .filter(|(k, _)| !k.is_empty())
        .collect()
}

/// The device's security floor. One that cannot be read is no floor to
/// install against: nothing installs.
pub(crate) fn security_floor() -> Result<u32, i32> {
    platform::floor()
}

/// The record NVS holds for `slot`, if any.
pub(crate) fn slot_record(slot: &str) -> Option<Record> {
    platform::get_record(slot)
}

/// A kernel has booted from `slot`, reporting `version`: record its slot,
/// and say what the floor is.
pub(crate) fn kernel_booted(slot: &str, version: &str) {
    #[cfg(target_os = "espidf")]
    let running = Record {
        version: version.to_string(),
        security_version: crate::version::SECURITY_VERSION,
    };
    // The simulator's "image" is the record the install wrote; only a slot
    // never written gets the build's.
    #[cfg(not(target_os = "espidf"))]
    let running = slot_record(slot).unwrap_or(Record {
        version: version.to_string(),
        security_version: crate::version::SECURITY_VERSION,
    });
    if slot_record(slot).as_ref() != Some(&running) && platform::record(slot, &running) != ESP_OK {
        log(ESP_LOG_WARN, &format!("could not record {slot}'s version in NVS"));
    }
    RUNNING_SECURITY.store(running.security_version, Ordering::Relaxed);
    match security_floor() {
        Ok(floor) => log(
            ESP_LOG_INFO,
            &format!(
                "security floor {floor}; this kernel's security version {}",
                running.security_version
            ),
        ),
        Err(err) => log(ESP_LOG_WARN, &format!("could not read the security floor: 0x{err:x}")),
    }
}

/// The running kernel has been kept: raise the floor to its security
/// version. Called by ota.rs, never for a kernel that is rolled back.
pub(crate) fn kernel_kept() {
    let running = RUNNING_SECURITY.load(Ordering::Relaxed);
    let floor = match security_floor() {
        Ok(floor) => floor,
        Err(err) => {
            log(ESP_LOG_ERROR, &format!("could not read the security floor: 0x{err:x}"));
            return;
        }
    };
    let raised = policy::floor_after_keep(floor, running);
    if raised == floor {
        return;
    }
    if platform::set_floor(raised) == ESP_OK {
        log(ESP_LOG_INFO, &format!("security floor raised from {floor} to {raised}"));
    } else {
        log(ESP_LOG_ERROR, &format!("could not raise the security floor to {raised}"));
    }
}

// ---------------------------------------------------------------------------
// A release on the card
// ---------------------------------------------------------------------------

/// A release copied to the card: its image, manifest and signature.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct CardRelease {
    pub bin: PathBuf,
    /// The version its manifest names, if the manifest reads at all.
    pub version: Option<String>,
}

impl CardRelease {
    fn manifest(&self) -> PathBuf {
        suffixed(&self.bin, ".manifest")
    }

    fn signature(&self) -> PathBuf {
        suffixed(&self.bin, ".manifest.sig")
    }
}

fn suffixed(path: &Path, suffix: &str) -> PathBuf {
    let mut p = path.as_os_str().to_owned();
    p.push(suffix);
    PathBuf::from(p)
}

fn update_dir() -> PathBuf {
    crate::appstore_client::card_root().join("update")
}

fn read_bounded(path: &Path, max: u64) -> Option<Vec<u8>> {
    let mut bytes = Vec::new();
    std::fs::File::open(path).ok()?.take(max + 1).read_to_end(&mut bytes).ok()?;
    (bytes.len() as u64 <= max).then_some(bytes)
}

/// The release in `dir` to install: of every `.bin` with a manifest beside
/// it, the one whose manifest names the newest version.
pub(crate) fn find_card_release(dir: &Path) -> Option<CardRelease> {
    let mut releases: Vec<CardRelease> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(|entry| entry.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|e| e == "bin") && p.is_file())
        .filter(|p| suffixed(p, ".manifest").is_file())
        .map(|bin| {
            let version = read_bounded(&suffixed(&bin, ".manifest"), MAX_MANIFEST)
                .and_then(|b| String::from_utf8(b).ok())
                .and_then(|text| ArtifactManifest::parse(&text).ok())
                .map(|m| m.version);
            CardRelease { bin, version }
        })
        .collect();
    releases.sort_by(|a, b| a.bin.cmp(&b.bin));
    let versions: Vec<Option<&str>> = releases.iter().map(|r| r.version.as_deref()).collect();
    let pick = policy::pick_card_release(&versions)?;
    Some(releases.swap_remove(pick))
}

pub(crate) fn card_release() -> Option<CardRelease> {
    find_card_release(&update_dir())
}

// ---------------------------------------------------------------------------
// The card install
// ---------------------------------------------------------------------------

enum Failure {
    Refused(Refusal),
    Error { what: String, err: i32 },
}

impl Failure {
    fn reason(&self) -> String {
        match self {
            Failure::Refused(refusal) => refusal.reason(),
            Failure::Error { what, .. } => what.clone(),
        }
    }

    fn code(&self) -> i32 {
        match self {
            Failure::Refused(Refusal::Signature | Refusal::Hash) => ESP_ERR_INVALID_CRC,
            Failure::Refused(Refusal::Size { .. }) => ESP_ERR_INVALID_SIZE,
            Failure::Refused(_) => ESP_ERR_NOT_SUPPORTED,
            Failure::Error { err, .. } => *err,
        }
    }
}

fn error(what: &str, err: i32) -> Failure {
    Failure::Error { what: what.to_string(), err }
}

fn device_now() -> Result<(String, u32), Failure> {
    let board = crate::board_config::board_id()
        .ok_or_else(|| error("this board's profile names no board id", ESP_ERR_NOT_FOUND))?;
    let floor = security_floor().map_err(|err| error("the security floor cannot be read", err))?;
    Ok((board, floor))
}

/// Check the card's release and write it into the other kernel slot.
/// Returns the version written.
fn install_card_release() -> Result<String, Failure> {
    let release = card_release()
        .ok_or_else(|| error("the card holds no release in update/", ESP_ERR_NOT_FOUND))?;
    let name = release.bin.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    log(ESP_LOG_INFO, &format!("card update: {name}"));

    let manifest_bytes = read_bounded(&release.manifest(), MAX_MANIFEST)
        .ok_or_else(|| error("its manifest cannot be read", ESP_ERR_NOT_FOUND))?;
    let signature = read_bounded(&release.signature(), 64).unwrap_or_default();
    let signature_ok = crate::signing::verify_firmware(&manifest_bytes, &signature);
    let manifest_text = String::from_utf8_lossy(&manifest_bytes);

    let (board, floor) = device_now()?;
    let device = Device { chip: crate::manifest::current_arch(), board: &board, floor };
    let manifest = policy::accept(signature_ok, &manifest_text, &device, &Source::Card)
        .map_err(Failure::Refused)?;

    let mut file = std::fs::File::open(&release.bin)
        .map_err(|_| error("its image cannot be read", ESP_ERR_NOT_FOUND))?;
    let size = file.metadata().map(|m| m.len()).unwrap_or(0);
    policy::check_size(&manifest, size).map_err(Failure::Refused)?;

    let running = crate::ota::running_slot()
        .ok_or_else(|| error("no running kernel slot", ESP_ERR_NOT_FOUND))?;
    let target = crate::kernel_slot::other_kernel_slot(running).ok_or_else(|| {
        error(&format!("running from {running}: flash this board by USB once"), ESP_ERR_NOT_FOUND)
    })?;

    log(
        ESP_LOG_INFO,
        &format!(
            "writing {} (security version {}, {} bytes) into {target}",
            manifest.version, manifest.security_version, size
        ),
    );
    with(|s| {
        s.version = manifest.version.clone();
        s.done = 0;
        s.total = size as u32;
    });

    // From here the other slot's previous kernel is gone, and going back
    // to it is no longer offered, whether or not this install finishes.
    if platform::record(target, &Record { version: String::new(), security_version: 0 }) != ESP_OK
    {
        log(ESP_LOG_WARN, &format!("could not clear {target}'s record in NVS"));
    }
    let mut writer = crate::ota::SlotWriter::begin(target, size as usize)
        .map_err(|err| error(&format!("{target} cannot be written"), err))?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; CHUNK];
    let mut written = 0u64;
    while written < size {
        let want = CHUNK.min((size - written) as usize);
        let count = match file.read(&mut buffer[..want]) {
            Ok(0) | Err(_) => {
                writer.abort();
                return Err(error("the image could not be read to its end", ESP_ERR_INVALID_SIZE));
            }
            Ok(count) => count,
        };
        hasher.update(&buffer[..count]);
        if let Err(err) = writer.write(&buffer[..count]) {
            writer.abort();
            return Err(error(&format!("{target} could not be written"), err));
        }
        written += count as u64;
        with(|s| s.done = written as u32);
    }
    let digest: [u8; 32] = hasher.finalize().into();
    if let Err(refusal) = policy::check_hash(&manifest, &digest) {
        writer.abort();
        return Err(Failure::Refused(refusal));
    }
    if let Err(err) = writer.finish() {
        log(
            ESP_LOG_ERROR,
            &format!("the image in {target} is not a kernel ESP-IDF can boot: 0x{err:x}"),
        );
        return Err(error("it is not a kernel this device can boot", err));
    }
    let record = Record {
        version: manifest.version.clone(),
        security_version: manifest.security_version,
    };
    if platform::record(target, &record) != ESP_OK {
        log(ESP_LOG_WARN, &format!("could not record {target}'s version in NVS"));
    }
    log(
        ESP_LOG_INFO,
        &format!("installed {} into {target}; it runs after the next restart", manifest.version),
    );
    Ok(manifest.version)
}

/// Run a card install to its end, as the task does, and say how it went.
fn run_card_install() -> i32 {
    // A sleep halfway through would leave half an image in the slot.
    let _awake = crate::power_manager::HoldGuard::new(b"an update is installing\0");
    let result = install_card_release();
    let code = match &result {
        Ok(version) => {
            with(|s| {
                s.state = STATE_READY;
                s.version = version.clone();
                s.reason.clear();
            });
            ESP_OK
        }
        Err(failure) => {
            let reason = failure.reason();
            log(ESP_LOG_WARN, &format!("card update refused: {reason}"));
            with(|s| {
                s.state = STATE_FAILED;
                s.reason = reason;
            });
            failure.code()
        }
    };
    BUSY.store(false, Ordering::Release);
    code
}

/// Claim the one install there may be at a time.
fn begin() -> bool {
    if BUSY.swap(true, Ordering::AcqRel) {
        return false;
    }
    with(|s| {
        s.state = STATE_WRITING;
        s.done = 0;
        s.total = 0;
        s.version.clear();
        s.reason.clear();
    });
    true
}

/// Install the card's release on the calling thread, as `--install-update`
/// and the App Store's old firmware path do.
pub(crate) fn install_from_card_blocking() -> i32 {
    if !begin() {
        return ESP_ERR_INVALID_STATE;
    }
    run_card_install()
}

#[cfg(target_os = "espidf")]
fn start_task() -> bool {
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
    }
    const PD_PASS: i32 = 1;
    const PRIORITY: u32 = 5;
    const NO_AFFINITY: i32 = 0x7fff_ffff;

    unsafe extern "C" fn entry(_arg: *mut c_void) {
        run_card_install();
        unsafe { vTaskDelete(std::ptr::null_mut()) };
    }

    unsafe {
        xTaskCreatePinnedToCore(
            entry,
            b"update\0".as_ptr() as *const c_char,
            STACK_SIZE as u32,
            std::ptr::null_mut(),
            PRIORITY,
            std::ptr::null_mut(),
            NO_AFFINITY,
        ) == PD_PASS
    }
}

#[cfg(not(target_os = "espidf"))]
fn start_task() -> bool {
    std::thread::Builder::new()
        .name("update".to_string())
        .stack_size(STACK_SIZE)
        .spawn(|| {
            run_card_install();
        })
        .is_ok()
}

// ---------------------------------------------------------------------------
// Going back
// ---------------------------------------------------------------------------

/// The other kernel slot and the version going back to it would reach, if
/// it is offered (update_policy::go_back). Not once an install has begun
/// erasing that slot, which clears its record, nor while one waits for its
/// restart: the slot is then the new kernel's.
pub(crate) fn go_back_target() -> Option<(&'static str, String)> {
    if with(|s| s.state) == STATE_READY {
        return None;
    }
    let other = crate::kernel_slot::other_kernel_slot(crate::ota::running_slot()?)?;
    let record = slot_record(other);
    let image = crate::ota::slot_image_version(other);
    let floor = security_floor().ok()?;
    let record = record.as_ref().map(|r| policy::SlotRecord {
        version: &r.version,
        security_version: r.security_version,
    });
    policy::go_back(record, image.as_deref(), floor).map(|version| (other, version))
}

/// Select the other kernel slot as it is and restart into it.
fn go_back() -> i32 {
    let Some((slot, version)) = go_back_target() else {
        return ESP_ERR_NOT_FOUND;
    };
    // Not during an install, and no install may start meanwhile.
    if BUSY.swap(true, Ordering::AcqRel) {
        return ESP_ERR_INVALID_STATE;
    }
    match crate::ota::select_slot(slot) {
        ESP_OK => {
            log(ESP_LOG_INFO, &format!("going back to {version} in {slot}; restarting"));
            crate::ota::restart()
        }
        err => {
            log(ESP_LOG_ERROR, &format!("could not select {slot} to go back: 0x{err:x}"));
            BUSY.store(false, Ordering::Release);
            err
        }
    }
}

// ---------------------------------------------------------------------------
// C API: thistle/update.h
// ---------------------------------------------------------------------------

/// update_status_t
#[repr(C)]
pub struct CUpdateStatus {
    pub state: u32,
    pub done: u32,
    pub total: u32,
    pub version: [c_char; 40],
    pub reason: [c_char; 128],
}

fn copy_c(text: &str, out: &mut [c_char]) {
    let mut n = text.len().min(out.len() - 1);
    while !text.is_char_boundary(n) {
        n -= 1;
    }
    for (dst, src) in out.iter_mut().zip(text.as_bytes()[..n].iter()) {
        *dst = *src as c_char;
    }
    out[n] = 0;
}

/// # Safety
/// `out` must be NULL or point to an update_status_t.
#[no_mangle]
pub unsafe extern "C" fn rs_update_get_status(out: *mut CUpdateStatus) {
    let Some(out) = out.as_mut() else { return };
    with(|s| {
        out.state = s.state;
        out.done = s.done;
        out.total = s.total;
        copy_c(&s.version, &mut out.version);
        copy_c(&s.reason, &mut out.reason);
    });
}

/// The running firmware's version.
#[no_mangle]
pub extern "C" fn rs_update_running_version() -> *const c_char {
    crate::ota::ota_get_current_version()
}

/// Whether the card holds a release in update/; its manifest's version into
/// `version` (empty if the manifest does not read).
///
/// # Safety
/// `version` must be NULL or point to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_update_card_release(version: *mut c_char, len: usize) -> bool {
    let Some(release) = card_release() else { return false };
    if !version.is_null() && len > 0 {
        let out = std::slice::from_raw_parts_mut(version, len);
        copy_c(release.version.as_deref().unwrap_or(""), out);
    }
    true
}

/// Start installing the card's release on a task of its own. Its progress
/// and outcome are in rs_update_get_status(). ESP_ERR_INVALID_STATE while
/// one is already under way.
#[no_mangle]
pub extern "C" fn rs_update_install_from_card() -> i32 {
    if !begin() {
        return ESP_ERR_INVALID_STATE;
    }
    if start_task() {
        return ESP_OK;
    }
    log(ESP_LOG_ERROR, "could not start a task to install the update");
    with(|s| {
        s.state = STATE_FAILED;
        s.reason = "no memory to start the install".into();
    });
    BUSY.store(false, Ordering::Release);
    ESP_FAIL
}

/// Restart into the kernel just installed. Nothing happens unless one is.
#[no_mangle]
pub extern "C" fn rs_update_restart() {
    if with(|s| s.state) == STATE_READY {
        log(ESP_LOG_INFO, "restarting into the new kernel");
        crate::ota::restart();
    }
}

/// Whether going back is offered; the version it would reach into
/// `version`.
///
/// # Safety
/// `version` must be NULL or point to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_update_go_back_version(version: *mut c_char, len: usize) -> bool {
    let Some((_, target)) = go_back_target() else { return false };
    if !version.is_null() && len > 0 {
        copy_c(&target, std::slice::from_raw_parts_mut(version, len));
    }
    true
}

/// Go back to the version in the other kernel slot: select it and restart.
/// Returns only if going back is not offered (ESP_ERR_NOT_FOUND) or fails.
#[no_mangle]
pub extern "C" fn rs_update_go_back() -> i32 {
    go_back()
}

// ---------------------------------------------------------------------------
// Tests: the decisions are update_policy's; test_system_update.py runs the
// install end to end in the simulator.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(case: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("thistle-update-{case}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn manifest(version: &str) -> String {
        format!(
            "THISTLE-ARTIFACT-MANIFEST-V1\ntype=firmware\nid=thistle-os\nversion={version}\n\
             security_version=500\narch=esp32s3\ncompatible_boards=tdeck-max\n\
             destination=kernel\nsha256={}\nsize=4\nurl=https://example.com/a.bin\n",
            "ab".repeat(32)
        )
    }

    fn put(dir: &Path, name: &str, manifest: Option<&str>) {
        std::fs::write(dir.join(name), b"test").unwrap();
        if let Some(m) = manifest {
            std::fs::write(dir.join(format!("{name}.manifest")), m).unwrap();
        }
    }

    #[test]
    fn a_card_with_no_update_folder_holds_no_release() {
        assert_eq!(find_card_release(&scratch("none").join("update")), None);
    }

    #[test]
    fn an_image_without_a_manifest_is_no_release() {
        let dir = scratch("bare");
        put(&dir, "thistle_os.bin", None);
        assert_eq!(find_card_release(&dir), None);
    }

    #[test]
    fn a_release_as_published_is_found_with_its_version() {
        let dir = scratch("one");
        put(&dir, "thistle_os-esp32s3-v0.6.0.bin", Some(&manifest("0.6.0")));
        let release = find_card_release(&dir).unwrap();
        assert_eq!(release.bin, dir.join("thistle_os-esp32s3-v0.6.0.bin"));
        assert_eq!(release.version.as_deref(), Some("0.6.0"));
        assert_eq!(
            release.signature(),
            dir.join("thistle_os-esp32s3-v0.6.0.bin.manifest.sig")
        );
    }

    #[test]
    fn of_two_releases_the_newer_is_found() {
        let dir = scratch("two");
        put(&dir, "thistle_os-esp32s3-v0.10.0.bin", Some(&manifest("0.10.0")));
        put(&dir, "thistle_os-esp32s3-v0.9.0.bin", Some(&manifest("0.9.0")));
        assert_eq!(find_card_release(&dir).unwrap().version.as_deref(), Some("0.10.0"));
    }

    #[test]
    fn a_release_whose_manifest_is_malformed_is_found_to_be_refused() {
        let dir = scratch("bad");
        put(&dir, "thistle_os.bin", Some("not a manifest"));
        let release = find_card_release(&dir).unwrap();
        assert_eq!(release.version, None);
    }

    #[test]
    fn the_simulators_nvs_reads_back_its_keys() {
        assert_eq!(
            parse_nvs("fw_sec_ver=600\nkernel_b_ver=0.6.0\njunk\n"),
            vec![
                ("fw_sec_ver".to_string(), "600".to_string()),
                ("kernel_b_ver".to_string(), "0.6.0".to_string())
            ]
        );
    }

    #[test]
    fn the_status_matches_its_c_layout() {
        // update_status_t: three uint32_t, char version[40], char reason[128].
        assert_eq!(std::mem::size_of::<CUpdateStatus>(), 12 + 40 + 128);
        assert_eq!(std::mem::offset_of!(CUpdateStatus, version), 12);
        assert_eq!(std::mem::offset_of!(CUpdateStatus, reason), 52);
    }

    #[test]
    fn the_status_reads_out_as_c_strings() {
        let mut out = [1 as c_char; 8];
        copy_c("0.6.0-rc.1", &mut out);
        assert_eq!(unsafe { CStr::from_ptr(out.as_ptr()) }.to_str().unwrap(), "0.6.0-r");
    }
}
