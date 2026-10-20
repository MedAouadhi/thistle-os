// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Kernel — elf_loader module
//
// Port of components/kernel/src/elf_loader.c
// Loads app ELF files into PSRAM using esp_elf, verifies signatures and
// parses manifests. An app is a shared object built by app_sdk/: its entry
// point, thistle_app_main, hands back the app's thistle_app_t, whose
// callbacks the app manager runs on the UI task like a built-in app's. The
// boot scan only loads and relocates an app; nothing of it runs until it is
// first opened, so an app that faults cannot stop the device booting. The
// ELF stays loaded from then on, as the callbacks live in it.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Mutex;

use crate::app_manager::{CAppEntry, CAppManifest};
use crate::ffi::CManifest;

// ---------------------------------------------------------------------------
// ESP-IDF error codes
// ---------------------------------------------------------------------------

const ESP_OK: i32 = 0x000;
const ESP_ERR_NO_MEM: i32 = 0x101;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
const ESP_ERR_INVALID_CRC: i32 = 0x109;
const ESP_FAIL: i32 = -1;

pub(crate) const MAX_LOADED_APPS: usize = 16;
const ELF_MAX_SIZE_BYTES: usize = 1024 * 1024; // 1 MB

// MALLOC_CAP_SPIRAM = BIT(10) (esp_heap_caps.h). BIT(9) is MALLOC_CAP_PID7,
// which no heap has, so every allocation asking for it failed.
const MALLOC_CAP_SPIRAM: u32 = 1 << 10;

// Permission flags (must match permissions.h)
const PERM_ALL: u32 = 0x7F;
const PERM_IPC: u32 = 1 << 6;

// ---------------------------------------------------------------------------
// C FFI declarations
// ---------------------------------------------------------------------------

extern "C" {
    // esp_elf
    fn esp_elf_init(elf: *mut c_void) -> i32;
    fn esp_elf_relocate(elf: *mut c_void, buf: *const u8) -> i32;
    fn esp_elf_request(elf: *mut c_void, opt: c_int, argc: c_int, argv: *mut *mut c_char) -> c_int;
    fn esp_elf_deinit(elf: *mut c_void);
    fn elf_set_symbol_resolver(resolver: unsafe extern "C" fn(*const c_char) -> usize);

    // PSRAM allocation
    fn heap_caps_malloc(size: usize, caps: u32) -> *mut c_void;
    fn free(ptr: *mut c_void);

    // Signing (Rust)
    fn signing_verify_app(data: *const u8, data_len: usize, signature: *const u8) -> i32;

    // Permissions (Rust)
    fn permissions_grant(app_id: *const c_char, perms: u32) -> i32;
    fn permissions_revoke(app_id: *const c_char, perms: u32) -> i32;

    // Manifest (C/Rust)
    fn manifest_parse_file(path: *const c_char, out: *mut CManifest) -> i32;
    fn manifest_path_from_elf(elf_path: *const c_char, out: *mut c_char, out_size: usize);

    // Syscall table
    fn syscall_table_count() -> usize;
    fn syscall_set_current_app(id: *const c_char, slot: i32);

    // Logging
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_INFO: i32 = 3;
const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_ERROR: i32 = 1;
const ESP_LOG_DEBUG: i32 = 4;

static TAG: &[u8] = b"elf_loader\0";

fn manifest_is_compatible_for_arch(manifest: &CManifest, current_arch: &CStr) -> bool {
    unsafe { crate::ffi::manifest_is_compatible(manifest, current_arch.as_ptr()) }
}

fn manifest_is_compatible_for_current_arch(manifest: &CManifest) -> bool {
    manifest_is_compatible_for_arch(manifest, crate::manifest::current_arch_cstr())
}

// Size of esp_elf_t opaque struct storage blob.
const ESP_ELF_T_SIZE: usize = 256;

// ---------------------------------------------------------------------------
// App handle type (opaque pointer exported to C)
// ---------------------------------------------------------------------------

/// What app_sdk's THISTLE_APP() declares (thistle_app.h), in the ELF.
#[repr(C)]
struct ThistleApp {
    id: *const c_char,
    name: *const c_char,
    version: *const c_char,
    allow_background: bool,
    on_create: Option<unsafe extern "C" fn() -> i32>,
    on_start: Option<unsafe extern "C" fn()>,
    on_pause: Option<unsafe extern "C" fn()>,
    on_resume: Option<unsafe extern "C" fn()>,
    on_destroy: Option<unsafe extern "C" fn()>,
}

/// The entry point app_sdk links every app with.
const APP_ENTRY_SYMBOL: &[u8] = b"thistle_app_main";

/// Internal ELF app state.
pub struct ElfAppHandle {
    elf_storage: [u8; ESP_ELF_T_SIZE],
    path: [u8; 128],
    /// The app's thistle_app_t, inside the loaded ELF: NULL until the app
    /// is first opened (opened_app).
    app: *const ThistleApp,
    loaded: bool,
    load_addr: usize,
    load_size: usize,
    app_id: [u8; 64],
    granted_permissions: u32,
    // Slot index in the global array
    slot: usize,
}

// SAFETY: Only accessed under Mutex.
unsafe impl Send for ElfAppHandle {}

impl ElfAppHandle {
    const fn empty() -> Self {
        ElfAppHandle {
            elf_storage: [0u8; ESP_ELF_T_SIZE],
            path: [0u8; 128],
            app: std::ptr::null(),
            loaded: false,
            load_addr: 0,
            load_size: 0,
            app_id: [0u8; 64],
            granted_permissions: 0,
            slot: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

struct ElfLoaderState {
    apps: Option<Box<[ElfAppHandle; MAX_LOADED_APPS]>>,
}

impl ElfLoaderState {
    const fn new() -> Self {
        ElfLoaderState { apps: None }
    }

    fn ensure_apps(&mut self) -> &mut [ElfAppHandle; MAX_LOADED_APPS] {
        if self.apps.is_none() {
            self.apps = Some(Box::new([
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
                ElfAppHandle::empty(),
            ]));
        }
        self.apps.as_mut().unwrap()
    }
}

// SAFETY: Only mutated through Mutex.
unsafe impl Send for ElfLoaderState {}

static STATE: Mutex<ElfLoaderState> = Mutex::new(ElfLoaderState::new());

fn effective_app_permissions(signed: bool, requested: u32) -> u32 {
    if signed {
        requested
    } else {
        PERM_IPC
    }
}

// ---------------------------------------------------------------------------
// Symbol resolver
// ---------------------------------------------------------------------------

unsafe extern "C" fn signed_app_symbol_resolver(sym_name: *const c_char) -> usize {
    let addr = crate::syscall_table::syscall_resolve_signed_app(sym_name);
    if addr.is_null() {
        esp_log_write(
            ESP_LOG_WARN,
            TAG.as_ptr(),
            b"Unresolved symbol: %s\0".as_ptr(),
            sym_name,
        );
        0
    } else {
        addr as usize
    }
}

unsafe extern "C" fn unsigned_app_symbol_resolver(sym_name: *const c_char) -> usize {
    crate::syscall_table::syscall_resolve_unsigned_app(sym_name) as usize
}

// ---------------------------------------------------------------------------
// The app's entry point
// ---------------------------------------------------------------------------

/// Whether the ELF in `elf` is entered at a global `thistle_app_main`, as
/// app_sdk links every app. Anything else, an ELF from before or a program
/// with a main(), would run whole when opening it called its entry point.
fn enters_at_app_main(elf: &[u8]) -> bool {
    fn u16_at(b: &[u8], at: usize) -> Option<usize> {
        Some(u16::from_le_bytes(b.get(at..at + 2)?.try_into().ok()?) as usize)
    }
    fn u32_at(b: &[u8], at: usize) -> Option<usize> {
        Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?) as usize)
    }
    const SHT_DYNSYM: usize = 11;
    let check = || -> Option<bool> {
        if elf.get(..5)? != b"\x7fELF\x01" {
            return Some(false);
        }
        let entry = u32_at(elf, 24)?;
        let (shoff, shentsize, shnum) = (u32_at(elf, 32)?, u16_at(elf, 46)?, u16_at(elf, 48)?);
        for i in 0..shnum {
            let sh = shoff + i * shentsize;
            if u32_at(elf, sh + 4)? != SHT_DYNSYM {
                continue;
            }
            let strtab = shoff + u32_at(elf, sh + 24)? * shentsize;
            let (str_off, str_size) = (u32_at(elf, strtab + 16)?, u32_at(elf, strtab + 20)?);
            let names = elf.get(str_off..str_off + str_size)?;
            let (sym_off, sym_size) = (u32_at(elf, sh + 16)?, u32_at(elf, sh + 20)?);
            for sym in (sym_off..sym_off + sym_size).step_by(16) {
                let name = names.get(u32_at(elf, sym)?..)?;
                let name = &name[..name.iter().position(|&b| b == 0)?];
                if name == APP_ENTRY_SYMBOL {
                    return Some(u32_at(elf, sym + 4)? == entry);
                }
            }
        }
        Some(false)
    };
    check().unwrap_or(false)
}

/// The app an ELF relocated in `elf_ptr` declares, from its entry point.
unsafe fn request_app(elf_ptr: *mut c_void) -> Option<&'static ThistleApp> {
    let mut argv: [*mut c_char; 1] = [std::ptr::null_mut()];
    if esp_elf_request(elf_ptr, 0, 1, argv.as_mut_ptr()) != 0 {
        return None;
    }
    (argv[0] as *const ThistleApp).as_ref()
}

// ---------------------------------------------------------------------------
// FFI exports
// ---------------------------------------------------------------------------

/// Initialise the ELF loader.
///
/// # Safety
/// May be called from C.
#[no_mangle]
pub extern "C" fn elf_loader_init() -> i32 {
    if let Ok(mut state) = STATE.lock() {
        let apps = state.ensure_apps();
        for (i, app) in apps.iter_mut().enumerate() {
            *app = ElfAppHandle::empty();
            app.slot = i;
        }
        unsafe {
            esp_log_write(
                ESP_LOG_INFO,
                TAG.as_ptr(),
                b"ELF loader initialised (max %d concurrent apps)\0".as_ptr(),
                MAX_LOADED_APPS as c_int,
            );
        }
    }
    ESP_OK
}

/// Load an app ELF from `path` into a free slot.
///
/// On success, `*handle` is set to a pointer into the global slot array.
///
/// # Safety
/// `path` must be a valid null-terminated C string.
/// `handle` must point to a valid `*mut ElfAppHandle` location.
#[no_mangle]
pub unsafe extern "C" fn elf_app_load(path: *const c_char, handle: *mut *mut ElfAppHandle) -> i32 {
    if path.is_null() || handle.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let path_str = match CStr::from_ptr(path).to_str() {
        Ok(s) => s,
        Err(_) => return ESP_ERR_INVALID_ARG,
    };

    // 1. Find a free slot
    let slot_idx = {
        let mut state = match STATE.lock() {
            Ok(s) => s,
            Err(_) => return ESP_FAIL,
        };
        let apps = state.ensure_apps();
        match apps.iter().position(|a| !a.loaded) {
            Some(i) => i,
            None => {
                esp_log_write(
                    ESP_LOG_ERROR,
                    TAG.as_ptr(),
                    b"No free ELF slots (max %d)\0".as_ptr(),
                    MAX_LOADED_APPS as c_int,
                );
                return ESP_ERR_NO_MEM;
            }
        }
    };

    // 2. Read ELF into PSRAM
    let file_data = match std::fs::read(path_str) {
        Ok(d) => d,
        Err(_) => {
            esp_log_write(
                ESP_LOG_ERROR,
                TAG.as_ptr(),
                b"Cannot open ELF: %s\0".as_ptr(),
                path,
            );
            return ESP_ERR_NOT_FOUND;
        }
    };

    let size = file_data.len();
    if size == 0 || size > ELF_MAX_SIZE_BYTES {
        esp_log_write(
            ESP_LOG_ERROR,
            TAG.as_ptr(),
            b"Rejecting ELF size %d: %s\0".as_ptr(),
            size as c_int,
            path,
        );
        return ESP_ERR_INVALID_SIZE;
    }

    let buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if buf.is_null() {
        return ESP_ERR_NO_MEM;
    }
    std::ptr::copy_nonoverlapping(file_data.as_ptr(), buf as *mut u8, size);
    drop(file_data);

    // Update the app slot with the load address and size
    {
        let mut state = match STATE.lock() {
            Ok(s) => s,
            Err(_) => {
                free(buf);
                return ESP_FAIL;
            }
        };
        let app = &mut state.ensure_apps()[slot_idx];
        app.load_addr = buf as usize;
        app.load_size = size;
    }

    // 3. Parse manifest FIRST to get the app ID for permission identity
    let mut app_id_buf = [0u8; 64];
    let mut has_manifest_id = false;
    let mut requested_permissions = 0u32;
    {
        let mut manifest_path_buf = [0u8; 280];
        manifest_path_from_elf(
            path,
            manifest_path_buf.as_mut_ptr() as *mut c_char,
            manifest_path_buf.len(),
        );
        let mut manifest = std::mem::MaybeUninit::<CManifest>::uninit();
        if manifest_parse_file(
            manifest_path_buf.as_ptr() as *const c_char,
            manifest.as_mut_ptr(),
        ) == ESP_OK
        {
            let manifest = manifest.assume_init();
            requested_permissions = manifest.permissions;
            let id_len = manifest
                .id
                .iter()
                .position(|&b| b == 0)
                .unwrap_or(63)
                .min(63);
            if id_len > 0 {
                app_id_buf[..id_len].copy_from_slice(&manifest.id[..id_len]);
                has_manifest_id = true;
            }
        }
    }

    // Use manifest ID for permissions if available, otherwise fall back to path
    let perm_id = if has_manifest_id {
        app_id_buf.as_ptr() as *const c_char
    } else {
        path
    };

    // 4. Verify signature on in-memory ELF data (avoids TOCTOU — no re-read from disk)
    let sig_ret = {
        let sig_path = format!("{}.sig", path_str);
        match std::fs::read(&sig_path) {
            Ok(sig_bytes) if sig_bytes.len() == 64 => {
                signing_verify_app(buf as *const u8, size, sig_bytes.as_ptr())
            }
            Ok(_) => ESP_ERR_INVALID_SIZE,
            Err(_) => ESP_ERR_NOT_FOUND,
        }
    };
    let signed = if sig_ret == ESP_OK {
        esp_log_write(
            ESP_LOG_INFO,
            TAG.as_ptr(),
            b"ELF signature verified: %s\0".as_ptr(),
            path,
        );
        true
    } else if sig_ret == ESP_ERR_NOT_FOUND {
        // Unsigned ELF — refuse in production; debug builds receive IPC only.
        #[cfg(not(debug_assertions))]
        {
            esp_log_write(
                ESP_LOG_ERROR,
                TAG.as_ptr(),
                b"ELF unsigned: %s (REFUSED - production)\0".as_ptr(),
                path,
            );
            free(buf);
            return sig_ret;
        }
        #[cfg(debug_assertions)]
        {
            esp_log_write(
                ESP_LOG_WARN,
                TAG.as_ptr(),
                b"ELF unsigned: %s (dev mode, IPC only)\0".as_ptr(),
                path,
            );
            false
        }
    } else {
        esp_log_write(
            ESP_LOG_ERROR,
            TAG.as_ptr(),
            b"ELF signature INVALID: %s\0".as_ptr(),
            path,
        );
        free(buf);
        return sig_ret;
    };

    if !enters_at_app_main(std::slice::from_raw_parts(buf as *const u8, size)) {
        esp_log_write(
            ESP_LOG_ERROR,
            TAG.as_ptr(),
            b"Not an app_sdk app (no thistle_app_main entry): %s\0".as_ptr(),
            path,
        );
        free(buf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    let granted_permissions = effective_app_permissions(signed, requested_permissions);

    // 4. Init esp_elf context
    let mut state = match STATE.lock() {
        Ok(s) => s,
        Err(_) => {
            free(buf);
            return ESP_FAIL;
        }
    };

    let app = &mut state.ensure_apps()[slot_idx];
    let elf_ptr = app.elf_storage.as_mut_ptr() as *mut c_void;

    let ret = esp_elf_init(elf_ptr);
    if ret != ESP_OK {
        esp_log_write(
            ESP_LOG_ERROR,
            TAG.as_ptr(),
            b"esp_elf_init failed: %s\0".as_ptr(),
            path,
        );
        free(buf);
        return ret;
    }

    // 5. Set resolver and relocate
    let sym_count = syscall_table_count();
    esp_log_write(
        ESP_LOG_INFO,
        TAG.as_ptr(),
        b"Relocating '%s' (%d bytes, %d syscalls)\0".as_ptr(),
        path,
        size as c_int,
        sym_count as c_int,
    );

    // Clear stale grants for a reused identity, then install only the trust-
    // appropriate permission set before relocation resolves imports.
    let _ = permissions_revoke(perm_id, PERM_ALL);
    let grant_ret = if granted_permissions == 0 {
        ESP_OK
    } else {
        permissions_grant(perm_id, granted_permissions)
    };
    if grant_ret != ESP_OK {
        free(buf);
        esp_elf_deinit(elf_ptr);
        return grant_ret;
    }

    elf_set_symbol_resolver(if signed {
        signed_app_symbol_resolver
    } else {
        unsigned_app_symbol_resolver
    });

    syscall_set_current_app(perm_id, slot_idx as i32);
    let ret = esp_elf_relocate(elf_ptr, buf as *const u8);
    syscall_set_current_app(std::ptr::null(), -1);

    free(buf);

    if ret != ESP_OK {
        esp_log_write(
            ESP_LOG_ERROR,
            TAG.as_ptr(),
            b"esp_elf_relocate failed: %s\0".as_ptr(),
            path,
        );
        let _ = permissions_revoke(perm_id, granted_permissions);
        esp_elf_deinit(elf_ptr);
        return ret;
    }

    // 6. Parse manifest alongside ELF
    {
        let mut manifest_path_buf = [0u8; 280];
        manifest_path_from_elf(
            path,
            manifest_path_buf.as_mut_ptr() as *mut c_char,
            manifest_path_buf.len(),
        );

        let mut manifest = std::mem::MaybeUninit::<CManifest>::uninit();
        if manifest_parse_file(
            manifest_path_buf.as_ptr() as *const c_char,
            manifest.as_mut_ptr(),
        ) == ESP_OK
        {
            let manifest = manifest.assume_init();
            if !manifest_is_compatible_for_current_arch(&manifest) {
                esp_log_write(
                    ESP_LOG_ERROR,
                    TAG.as_ptr(),
                    b"App incompatible: %s\0".as_ptr(),
                    path,
                );
                let _ = permissions_revoke(perm_id, granted_permissions);
                esp_elf_deinit(elf_ptr);
                return ESP_ERR_NOT_SUPPORTED;
            }
            esp_log_write(
                ESP_LOG_INFO,
                TAG.as_ptr(),
                b"Manifest OK: %s\0".as_ptr(),
                path,
            );
        }
    }

    // 7. Store metadata
    let path_bytes = CStr::from_ptr(path).to_bytes_with_nul();
    let copy_len = path_bytes.len().min(app.path.len() - 1);
    app.path[..copy_len].copy_from_slice(&path_bytes[..copy_len]);
    app.path[copy_len] = 0;
    let identity_bytes = CStr::from_ptr(perm_id).to_bytes();
    let identity_len = identity_bytes.len().min(app.app_id.len() - 1);
    app.app_id[..identity_len].copy_from_slice(&identity_bytes[..identity_len]);
    app.app_id[identity_len] = 0;
    app.granted_permissions = granted_permissions;
    app.app = std::ptr::null();
    app.loaded = true;
    app.slot = slot_idx;

    *handle = app as *mut ElfAppHandle;

    esp_log_write(
        ESP_LOG_INFO,
        TAG.as_ptr(),
        b"ELF loaded: %s\0".as_ptr(),
        path,
    );
    ESP_OK
}

/// Unload a loaded ELF app. Only for one that failed to register: a
/// registered app's callbacks live in its ELF.
///
/// # Safety
/// `handle` must have been obtained from `elf_app_load`.
#[no_mangle]
pub unsafe extern "C" fn elf_app_unload(handle: *mut ElfAppHandle) -> i32 {
    if handle.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let app = &mut *handle;

    // Deinit ELF
    if app.loaded {
        let elf_ptr = app.elf_storage.as_mut_ptr() as *mut c_void;
        esp_elf_deinit(elf_ptr);
        app.loaded = false;
    }

    if app.granted_permissions != 0 && app.app_id[0] != 0 {
        let _ = permissions_revoke(
            app.app_id.as_ptr() as *const c_char,
            app.granted_permissions,
        );
        app.granted_permissions = 0;
    }

    crate::syscall_table::syscall_clear_app_storage(app.slot);

    esp_log_write(ESP_LOG_INFO, TAG.as_ptr(), b"ELF app unloaded\0".as_ptr());

    // Clear the slot
    let slot = app.slot;
    if let Ok(mut state) = STATE.lock() {
        let apps = state.ensure_apps();
        apps[slot] = ElfAppHandle::empty();
        apps[slot].slot = slot;
    }

    ESP_OK
}

/// Return a pointer to the app's manifest struct, or NULL if not loaded.
///
/// # Safety
/// `handle` must have been obtained from `elf_app_load`.
/// Returns NULL if the handle is invalid or the app is not loaded.
#[no_mangle]
pub unsafe extern "C" fn elf_app_get_manifest(handle: *const ElfAppHandle) -> *const c_void {
    if handle.is_null() {
        return std::ptr::null();
    }
    let app = &*handle;
    if !app.loaded {
        return std::ptr::null();
    }
    // Return a pointer to the path as a minimal manifest-like identification.
    // The real manifest fields are stored in the C-side app_manifest_t which
    // is embedded in the ELF's .thistle_app section — the caller already has
    // that after elf_app_load returns successfully.
    app.path.as_ptr() as *const c_void
}

// ---------------------------------------------------------------------------
// Scan & Register — wires ELF apps into the app manager
// ---------------------------------------------------------------------------

/// Static storage for app registration metadata.
/// The app manager holds raw pointers into these, so they must be 'static.
struct ElfAppRegistration {
    manifest: CAppManifest,
    entry: CAppEntry,
    handle: *mut ElfAppHandle,
    used: bool,
}

impl ElfAppRegistration {
    const fn empty() -> Self {
        ElfAppRegistration {
            manifest: CAppManifest {
                id: std::ptr::null(),
                name: std::ptr::null(),
                version: b"0.0.0\0".as_ptr() as *const c_char,
                allow_background: false,
                min_memory_kb: 0,
            },
            entry: CAppEntry {
                on_create: None,
                on_start: None,
                on_pause: None,
                on_resume: None,
                on_destroy: None,
                manifest: std::ptr::null(),
            },
            handle: std::ptr::null_mut(),
            used: false,
        }
    }
}

// SAFETY: Only accessed under REG_MUTEX.
unsafe impl Send for ElfAppRegistration {}

static REG_MUTEX: Mutex<Option<Box<[ElfAppRegistration; MAX_LOADED_APPS]>>> = Mutex::new(None);

fn ensure_regs(
    guard: &mut Option<Box<[ElfAppRegistration; MAX_LOADED_APPS]>>,
) -> &mut [ElfAppRegistration; MAX_LOADED_APPS] {
    if guard.is_none() {
        *guard = Some(Box::new([
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
            ElfAppRegistration::empty(),
        ]));
    }
    guard.as_mut().unwrap()
}

/// Static string storage for manifest id/name fields.
/// Each registration slot gets its own id and name buffer that lives forever.
struct RegStrings {
    id: [u8; 64],
    name: [u8; 32],
    version: [u8; 16],
}

impl RegStrings {
    const fn empty() -> Self {
        RegStrings {
            id: [0u8; 64],
            name: [0u8; 32],
            version: [0u8; 16],
        }
    }
}

// SAFETY: Only accessed under REG_STR_MUTEX.
unsafe impl Send for RegStrings {}

static REG_STR_MUTEX: Mutex<Option<Box<[RegStrings; MAX_LOADED_APPS]>>> = Mutex::new(None);

fn ensure_reg_strings(
    guard: &mut Option<Box<[RegStrings; MAX_LOADED_APPS]>>,
) -> &mut [RegStrings; MAX_LOADED_APPS] {
    if guard.is_none() {
        *guard = Some(Box::new([
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
            RegStrings::empty(),
        ]));
    }
    guard.as_mut().unwrap()
}

/// The app registered in `slot`, asking it for its callbacks the first time
/// (thistle_app_main). Its id must be its manifest's, which its permissions
/// were granted to.
unsafe fn opened_app(slot: usize) -> Option<&'static ThistleApp> {
    let handle = REG_MUTEX.lock().ok()?.as_ref()?[slot].handle;
    if handle.is_null() {
        return None;
    }
    let loaded = &mut *handle;
    if loaded.app.is_null() {
        let app = request_app(loaded.elf_storage.as_mut_ptr() as *mut c_void);
        let manifest_id = CStr::from_ptr(loaded.app_id.as_ptr() as *const c_char);
        match app {
            Some(app) if !app.id.is_null() && CStr::from_ptr(app.id) == manifest_id => {
                loaded.app = app;
                esp_log_write(
                    ESP_LOG_INFO,
                    TAG.as_ptr(),
                    b"Opened ELF app: %s\0".as_ptr(),
                    app.id,
                );
            }
            _ => {
                esp_log_write(
                    ESP_LOG_ERROR,
                    TAG.as_ptr(),
                    b"App does not declare itself as its manifest does: %s\0".as_ptr(),
                    manifest_id.as_ptr(),
                );
                return None;
            }
        }
    }
    Some(&*loaded.app)
}

// The callbacks registered for each slot: each opens the app in it and calls
// the app's own.
unsafe extern "C" fn on_create_in<const SLOT: usize>() -> i32 {
    match opened_app(SLOT) {
        Some(app) => app.on_create.map_or(ESP_OK, |on_create| on_create()),
        None => ESP_ERR_INVALID_STATE,
    }
}

unsafe extern "C" fn on_start_in<const SLOT: usize>() {
    if let Some(on_start) = opened_app(SLOT).and_then(|app| app.on_start) {
        on_start();
    }
}

unsafe extern "C" fn on_pause_in<const SLOT: usize>() {
    if let Some(on_pause) = opened_app(SLOT).and_then(|app| app.on_pause) {
        on_pause();
    }
}

unsafe extern "C" fn on_resume_in<const SLOT: usize>() {
    if let Some(on_resume) = opened_app(SLOT).and_then(|app| app.on_resume) {
        on_resume();
    }
}

unsafe extern "C" fn on_destroy_in<const SLOT: usize>() {
    if let Some(on_destroy) = opened_app(SLOT).and_then(|app| app.on_destroy) {
        on_destroy();
    }
}

macro_rules! per_slot {
    ($callback:ident) => {
        [
            $callback::<0>, $callback::<1>, $callback::<2>, $callback::<3>,
            $callback::<4>, $callback::<5>, $callback::<6>, $callback::<7>,
            $callback::<8>, $callback::<9>, $callback::<10>, $callback::<11>,
            $callback::<12>, $callback::<13>, $callback::<14>, $callback::<15>,
        ]
    };
}

const ON_CREATE: [unsafe extern "C" fn() -> i32; MAX_LOADED_APPS] = per_slot!(on_create_in);
const ON_START: [unsafe extern "C" fn(); MAX_LOADED_APPS] = per_slot!(on_start_in);
const ON_PAUSE: [unsafe extern "C" fn(); MAX_LOADED_APPS] = per_slot!(on_pause_in);
const ON_RESUME: [unsafe extern "C" fn(); MAX_LOADED_APPS] = per_slot!(on_resume_in);
const ON_DESTROY: [unsafe extern "C" fn(); MAX_LOADED_APPS] = per_slot!(on_destroy_in);

/// The app ELFs under `dir`: each `NAME.app.elf`, and each app the App Store
/// installed, `ID/generations/N/app.app.elf` for the release `ID/active.json`
/// names. Sorted, so apps register in the same order every boot.
pub fn app_elf_paths(dir: &std::path::Path) -> std::io::Result<Vec<std::path::PathBuf>> {
    let mut found = Vec::new();
    for entry in std::fs::read_dir(dir)?.flatten() {
        let path = entry.path();
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with('.') {
            continue;
        }
        if name.ends_with(".app.elf") && path.is_file() {
            found.push(path);
        } else if path.is_dir() {
            if let Some(generation) = crate::tap_installer::active_generation(&path) {
                found.push(generation.join("app.app.elf"));
            }
        }
    }
    found.sort();
    Ok(found)
}

/// Scan `/spiffs/apps/` and `/sdcard/apps/` for app ELFs (`app_elf_paths`),
/// load each one, and register it with the app manager so it appears in the
/// launcher.
///
/// # Safety
/// May be called from C. Accesses the filesystem and global state.
#[no_mangle]
pub unsafe extern "C" fn elf_app_scan_and_register() -> c_int {
    let dirs: [&str; 2] = ["/spiffs/apps\0", "/sdcard/apps\0"];
    let mut registered: i32 = 0;

    for dir in &dirs {
        let elf_paths = match app_elf_paths(std::path::Path::new(dir.trim_end_matches('\0'))) {
            Ok(found) => found,
            Err(_) => {
                esp_log_write(
                    ESP_LOG_DEBUG,
                    TAG.as_ptr(),
                    b"No apps directory: %s\0".as_ptr(),
                    dir.as_ptr(),
                );
                continue;
            }
        };

        for elf_path in elf_paths {
            let full_path = format!("{}\0", elf_path.display());
            let full_path_ptr = full_path.as_ptr() as *const c_char;

            esp_log_write(
                ESP_LOG_INFO,
                TAG.as_ptr(),
                b"Found app ELF: %s\0".as_ptr(),
                full_path_ptr,
            );

            // 1. Load the ELF
            let mut handle: *mut ElfAppHandle = std::ptr::null_mut();
            let ret = elf_app_load(full_path_ptr, &mut handle);
            if ret != ESP_OK {
                esp_log_write(
                    ESP_LOG_WARN,
                    TAG.as_ptr(),
                    b"Failed to load app ELF '%s': 0x%x\0".as_ptr(),
                    full_path_ptr,
                    ret,
                );
                continue;
            }

            // 2. Parse manifest to get app metadata
            let mut manifest_path_buf = [0u8; 280];
            manifest_path_from_elf(
                full_path_ptr,
                manifest_path_buf.as_mut_ptr() as *mut c_char,
                manifest_path_buf.len(),
            );

            let mut c_manifest = std::mem::zeroed::<CManifest>();
            let manifest_ret =
                manifest_parse_file(manifest_path_buf.as_ptr() as *const c_char, &mut c_manifest);

            if manifest_ret != ESP_OK {
                esp_log_write(
                    ESP_LOG_WARN,
                    TAG.as_ptr(),
                    b"No manifest for app ELF '%s', skipping registration\0".as_ptr(),
                    full_path_ptr,
                );
                let _ = elf_app_unload(handle);
                continue;
            }

            // Extract id and name from the parsed manifest
            let id_len = c_manifest
                .id
                .iter()
                .position(|&b| b == 0)
                .unwrap_or(c_manifest.id.len());
            let name_len = c_manifest
                .name
                .iter()
                .position(|&b| b == 0)
                .unwrap_or(c_manifest.name.len());

            if id_len == 0 {
                esp_log_write(
                    ESP_LOG_WARN,
                    TAG.as_ptr(),
                    b"App ELF '%s' has empty manifest id, skipping\0".as_ptr(),
                    full_path_ptr,
                );
                let _ = elf_app_unload(handle);
                continue;
            }

            // 3. Find a free registration slot
            let reg_slot = {
                let mut regs = match REG_MUTEX.lock() {
                    Ok(r) => r,
                    Err(_) => {
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                };
                let r = ensure_regs(&mut regs);
                match r.iter().position(|r| !r.used) {
                    Some(i) => i,
                    None => {
                        esp_log_write(
                            ESP_LOG_ERROR,
                            TAG.as_ptr(),
                            b"No free registration slots for app '%s'\0".as_ptr(),
                            full_path_ptr,
                        );
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                }
            };

            // 4. Copy id/name into static string storage so pointers remain valid
            {
                let mut strings = match REG_STR_MUTEX.lock() {
                    Ok(s) => s,
                    Err(_) => {
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                };
                let s = &mut ensure_reg_strings(&mut strings)[reg_slot];
                let copy_id = id_len.min(s.id.len() - 1);
                s.id[..copy_id].copy_from_slice(&c_manifest.id[..copy_id]);
                s.id[copy_id] = 0;
                let copy_name = name_len.min(s.name.len() - 1);
                s.name[..copy_name].copy_from_slice(&c_manifest.name[..copy_name]);
                s.name[copy_name] = 0;
                let version_len = c_manifest.version.iter().position(|&b| b == 0).unwrap_or(0);
                let copy_version = version_len.min(s.version.len() - 1);
                s.version[..copy_version].copy_from_slice(&c_manifest.version[..copy_version]);
                s.version[copy_version] = 0;
            }

            // 5. Build registration entry with static pointers
            //    We must get pointers into the static string storage.
            let (id_ptr, name_ptr, version_ptr) = {
                let mut strings = match REG_STR_MUTEX.lock() {
                    Ok(s) => s,
                    Err(_) => {
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                };
                let strs = ensure_reg_strings(&mut strings);
                (
                    strs[reg_slot].id.as_ptr() as *const c_char,
                    strs[reg_slot].name.as_ptr() as *const c_char,
                    strs[reg_slot].version.as_ptr() as *const c_char,
                )
            };

            {
                let mut regs = match REG_MUTEX.lock() {
                    Ok(r) => r,
                    Err(_) => {
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                };
                let reg = &mut ensure_regs(&mut regs)[reg_slot];

                // The slot's callbacks, which open the app and call its own
                // on the UI task, as the simulator calls a host build's
                // (sim_app_loader.c).
                reg.manifest = CAppManifest {
                    id: id_ptr,
                    name: name_ptr,
                    version: version_ptr,
                    allow_background: c_manifest.background,
                    min_memory_kb: c_manifest.min_memory_kb,
                };
                reg.handle = handle;
                reg.entry = CAppEntry {
                    on_create: Some(ON_CREATE[reg_slot]),
                    on_start: Some(ON_START[reg_slot]),
                    on_pause: Some(ON_PAUSE[reg_slot]),
                    on_resume: Some(ON_RESUME[reg_slot]),
                    on_destroy: Some(ON_DESTROY[reg_slot]),
                    manifest: &reg.manifest as *const CAppManifest,
                };
                reg.used = true;
            }

            // 6. Register with the app manager
            let entry_ptr = {
                let mut regs = match REG_MUTEX.lock() {
                    Ok(r) => r,
                    Err(_) => {
                        let _ = elf_app_unload(handle);
                        continue;
                    }
                };
                &ensure_regs(&mut regs)[reg_slot].entry as *const CAppEntry
            };

            let reg_ret = crate::app_manager::register(entry_ptr);
            if reg_ret != ESP_OK {
                esp_log_write(
                    ESP_LOG_WARN,
                    TAG.as_ptr(),
                    b"Failed to register app '%s' with app manager: 0x%x\0".as_ptr(),
                    full_path_ptr,
                    reg_ret,
                );
                // Roll back the slot
                if let Ok(mut regs) = REG_MUTEX.lock() {
                    ensure_regs(&mut regs)[reg_slot] = ElfAppRegistration::empty();
                }
                let _ = elf_app_unload(handle);
                continue;
            }

            // Permissions were fixed before relocation from signature trust
            // and the manifest. Never re-grant raw manifest bits here, since
            // an unsigned debug app must remain IPC-only.
            let perms = (*handle).granted_permissions;

            esp_log_write(
                ESP_LOG_INFO,
                TAG.as_ptr(),
                b"Registered ELF app: %s %s (perms=0x%x)\0".as_ptr(),
                id_ptr,
                version_ptr,
                perms,
            );
            registered += 1;
        }
    }

    esp_log_write(
        ESP_LOG_INFO,
        TAG.as_ptr(),
        b"App scan complete: %d app(s) registered\0".as_ptr(),
        registered,
    );

    registered
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn scan_finds_plain_elfs_and_active_store_installs_only() {
        let apps = std::env::temp_dir().join(format!("thistle-elf-scan-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&apps);
        std::fs::create_dir_all(&apps).unwrap();
        for file in ["hello.app.elf", "hello.app.elf.sig", "hello.manifest.json", ".hello.app.elf.thistle-tmp-1"] {
            std::fs::write(apps.join(file), b"x").unwrap();
        }
        // Installed and activated.
        let notes = apps.join("com.thistle.notes");
        std::fs::create_dir_all(notes.join("generations/2")).unwrap();
        std::fs::write(notes.join("generations/2/app.app.elf"), b"elf").unwrap();
        std::fs::write(notes.join("active.json"), "{\"release_sequence\": 2, \"version\": \"1.1.0\"}").unwrap();
        // Unpacked, never activated.
        std::fs::create_dir_all(apps.join("com.thistle.half/generations/1")).unwrap();
        std::fs::write(apps.join("com.thistle.half/generations/1/app.app.elf"), b"elf").unwrap();

        assert_eq!(
            app_elf_paths(&apps).unwrap(),
            vec![notes.join("generations/2/app.app.elf"), apps.join("hello.app.elf")]
        );
        std::fs::remove_dir_all(&apps).unwrap();
    }

    /// An ELF32 header and three sections: none, .dynsym, .dynstr, the
    /// symbols `names` at `values`, entered at `entry`.
    fn elf_with_symbols(entry: u32, symbols: &[(&str, u32)]) -> Vec<u8> {
        let mut strings = vec![0u8];
        let mut syms = vec![0u8; 16];
        for (name, value) in symbols {
            let mut sym = [0u8; 16];
            sym[..4].copy_from_slice(&(strings.len() as u32).to_le_bytes());
            sym[4..8].copy_from_slice(&value.to_le_bytes());
            syms.extend_from_slice(&sym);
            strings.extend_from_slice(name.as_bytes());
            strings.push(0);
        }
        let mut elf = vec![0u8; 52];
        elf[..5].copy_from_slice(b"\x7fELF\x01");
        elf[24..28].copy_from_slice(&entry.to_le_bytes());
        let (sym_off, str_off) = (elf.len(), elf.len() + syms.len());
        elf.extend_from_slice(&syms);
        elf.extend_from_slice(&strings);
        let shoff = elf.len();
        elf[32..36].copy_from_slice(&(shoff as u32).to_le_bytes());
        elf[46..48].copy_from_slice(&40u16.to_le_bytes());
        elf[48..50].copy_from_slice(&3u16.to_le_bytes());
        let section = |kind: u32, offset: usize, size: usize, link: u32| {
            let mut sh = [0u8; 40];
            sh[4..8].copy_from_slice(&kind.to_le_bytes());
            sh[16..20].copy_from_slice(&(offset as u32).to_le_bytes());
            sh[20..24].copy_from_slice(&(size as u32).to_le_bytes());
            sh[24..28].copy_from_slice(&link.to_le_bytes());
            sh
        };
        elf.extend_from_slice(&[0u8; 40]);
        elf.extend_from_slice(&section(11, sym_off, syms.len(), 2));
        elf.extend_from_slice(&section(3, str_off, strings.len(), 0));
        elf
    }

    #[test]
    fn only_an_elf_entered_at_thistle_app_main_is_an_app() {
        let app = elf_with_symbols(0x1acc, &[("thistle_ui_set_text", 0), ("thistle_app_main", 0x1acc)]);
        assert!(enters_at_app_main(&app));
        // A program entered at its main().
        assert!(!enters_at_app_main(&elf_with_symbols(0x200, &[("thistle_app_main", 0x1acc), ("main", 0x200)])));
        assert!(!enters_at_app_main(&elf_with_symbols(0x200, &[("main", 0x200)])));
        assert!(!enters_at_app_main(&app[..app.len() - 30]), "cut short");
        assert!(!enters_at_app_main(b"\x7fELF"));
        assert!(!enters_at_app_main(&[]));
    }

    #[test]
    fn unsigned_manifest_cannot_escalate_beyond_ipc() {
        assert_eq!(effective_app_permissions(false, PERM_ALL), PERM_IPC);
        assert_eq!(effective_app_permissions(false, 0), PERM_IPC);
    }

    #[test]
    fn signed_manifest_keeps_only_requested_permissions() {
        assert_eq!(effective_app_permissions(true, PERM_ALL), PERM_ALL);
        assert_eq!(effective_app_permissions(true, PERM_IPC), PERM_IPC);
        assert_eq!(effective_app_permissions(true, 0), 0);
    }

    #[test]
    fn empty_handle_owns_no_identity_or_permissions() {
        let app = ElfAppHandle::empty();
        assert_eq!(app.app_id, [0u8; 64]);
        assert_eq!(app.granted_permissions, 0);
    }

    #[test]
    fn app_loader_accepts_matching_canonical_architectures_only() {
        let universal = CManifest::from(&crate::manifest::Manifest::default());
        for package_arch in ["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6"] {
            let manifest = CManifest::from(&crate::manifest::Manifest {
                arch: package_arch.into(),
                ..Default::default()
            });
            let matching = CString::new(package_arch).unwrap();
            assert!(manifest_is_compatible_for_arch(&universal, &matching));
            assert!(manifest_is_compatible_for_arch(&manifest, &matching));

            let mismatch = if package_arch == "esp32c3" {
                CString::new("esp32s3").unwrap()
            } else {
                CString::new("esp32c3").unwrap()
            };
            assert!(!manifest_is_compatible_for_arch(&manifest, &mismatch));
        }
    }
}
