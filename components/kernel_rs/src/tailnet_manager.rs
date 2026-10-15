// SPDX-License-Identifier: BSD-3-Clause
//! Keeps the device on the user's tailnet (Tailscale) while they want it.
//!
//! What to do is decided in tailnet_policy.rs; this runs it. The client is
//! MicroLink, behind tailnet_platform.c, and it runs four tasks of its own
//! while it is up. Once on the tailnet, any socket reaches a peer's
//! 100.x.y.z address, and getaddrinfo() resolves the tailnet's names.
//!
//! - A kernel worker job looks once a second: is the switch on, is there a
//!   network and which, is the clock set, what is the client doing. It
//!   never waits: what the client is doing is read in one poll that only
//!   tries the platform's lock, and kept for the status, the peer list and
//!   the Terminal, which so never touch the client. Starting the client and
//!   stopping it are done on a task of their own, started for the purpose
//!   and gone when done, since stopping waits for the client's tasks, up to
//!   30 s.
//! - The switch is `thistle_os.tailnet.enabled` (off unless switched on),
//!   the name `thistle_os.tailnet.hostname` ("" for thistle-xxxxxx, from the
//!   MAC address).
//! - Pairing needs nothing compiled in: the control plane gives a URL to
//!   sign in at, which the Tailscale app shows as a QR code. While one is
//!   waiting the device is held awake, for up to ten minutes.
//! - The node's identity is the client's, in NVS, and survives a restart; a
//!   device that has joined rejoins with it and needs neither again. "Sign
//!   out" erases it; the device stays in the admin console until removed
//!   there.

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Mutex, MutexGuard, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use crate::power_manager::HoldGuard;
use crate::tailnet_policy::{self as policy, Action, Client, Inputs, Shown, Underlay};

const SECTION: &str = "tailnet";

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

/// How long a sign-in waiting at a URL may keep the device awake.
const LOGIN_HOLD_MS: u64 = 10 * 60 * 1000;
/// The task that starts and stops the client. Its stack is in internal RAM:
/// starting the client reads and writes NVS.
const STACK_SIZE: usize = 6144;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"tailnet\0";

#[cfg(not(test))]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}
#[cfg(test)]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        let _ = ($level, $fmt $(, $arg)*);
    }};
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

// ---------------------------------------------------------------------------
// Settings in system.json
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Settings {
    pub enabled: bool,
    /// As saved; "" for the default.
    pub hostname: String,
}

pub(crate) fn settings_in(json: Option<&str>) -> Settings {
    let get = |key| json.and_then(|j| crate::system_json::get(j, SECTION, key));
    Settings {
        enabled: get("enabled").and_then(|v| v.as_bool()).unwrap_or(false),
        hostname: get("hostname")
            .and_then(|v| v.as_str().and_then(policy::hostname_from))
            .unwrap_or_default(),
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct State {
    enabled: bool,
    hostname: String,
    /// The network the running client was started on; None if not running.
    running_on: Option<Underlay>,
    busy: bool,
    refused: bool,
    restart: bool,
    forget: bool,
    failures: u32,
    retry_at_ms: u64,
    /// What stopped it, for the user: a start that failed, or a refusal.
    failure: String,
    login_since_ms: Option<u64>,
    hold: Option<HoldGuard>,
    shown: Shown,
    /// What the client was last seen doing; None while it is not running.
    polled: Option<Polled>,
}

/// What a poll of the running client found.
#[derive(Clone, Debug)]
pub(crate) struct Polled {
    pub client: Client,
    pub error: i32,
    pub error_text: String,
    pub ip: u32,
    pub auth_url: String,
    /// (address, name, reached directly)
    pub peers: Vec<(u32, String, bool)>,
}

/// What a poll found.
pub(crate) enum Poll {
    NotRunning,
    /// Starting, stopping, or a name lookup: what was seen last still holds
    Busy,
    Running(Polled),
}

impl State {
    fn client(&self) -> Client {
        self.polled.as_ref().map_or(Client::NotRunning, |p| p.client)
    }
}

static STATE: Mutex<Option<State>> = Mutex::new(None);

fn lock() -> MutexGuard<'static, Option<State>> {
    STATE.lock().unwrap_or_else(PoisonError::into_inner)
}

fn with<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    lock().as_mut().map(f)
}

fn now_ms() -> u64 {
    static START: OnceLock<Instant> = OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_millis() as u64
}

fn clock_known() -> bool {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_micros() as i64)
        .unwrap_or(0);
    crate::time_policy::plausible(now)
}

/// The network traffic goes over, as net_manager names it.
fn underlay() -> Option<Underlay> {
    let drv = crate::net_manager::net_get_active_underlay();
    if drv.is_null() {
        return None;
    }
    let d = unsafe { &*drv };
    let ip = d
        .get_ip
        .map(|f| unsafe { f() })
        .filter(|p| !p.is_null())
        .map(|p| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
        .unwrap_or_default();
    Some(Underlay { kind: d.transport_type, ip })
}

fn hostname_or_default(saved: &str) -> String {
    if !saved.is_empty() {
        return saved.to_string();
    }
    let mut buf = [0u8; 64];
    platform::default_name(&mut buf);
    let name = CStr::from_bytes_until_nul(&buf)
        .map(|c| c.to_string_lossy().into_owned())
        .unwrap_or_default();
    policy::hostname_from(&name).unwrap_or_else(|| "thistle".to_string())
}

// ---------------------------------------------------------------------------
// Starting and stopping, on a task of their own
// ---------------------------------------------------------------------------

enum Job {
    Start(Underlay),
    Stop,
    Restart(Underlay),
    Forget,
}

fn spawn(job: Job) {
    let forget = matches!(job, Job::Forget);
    if !start_task(job) {
        log!(ESP_LOG_WARN, b"could not start a task to start or stop the client\0");
        with(|s| {
            s.busy = false;
            if forget {
                // Asked for again at the next look, rather than lost
                s.forget = true;
            } else {
                s.failures += 1;
                s.retry_at_ms = now_ms() + policy::backoff_ms(s.failures);
            }
        });
    }
}

/// Run `job` on a FreeRTOS task of its own, gone when done. Not a
/// std::thread: on ESP-IDF one leaves its Thread handle behind when it
/// ends, about 150 bytes of internal RAM, and the client is started and
/// stopped at every change of network, and so at every sleep.
#[cfg(target_os = "espidf")]
fn start_task(job: Job) -> bool {
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
    const PRIORITY: u32 = 5; // as a pthread's (CONFIG_PTHREAD_TASK_PRIO_DEFAULT)
    const NO_AFFINITY: i32 = 0x7fff_ffff;

    unsafe extern "C" fn entry(arg: *mut c_void) {
        {
            // Dropped here: nothing after vTaskDelete() runs.
            let job = unsafe { Box::from_raw(arg as *mut Job) };
            run(*job);
        }
        unsafe { vTaskDelete(std::ptr::null_mut()) };
    }

    let arg = Box::into_raw(Box::new(job)) as *mut c_void;
    let created = unsafe {
        xTaskCreatePinnedToCore(entry, b"tailnet\0".as_ptr() as *const c_char, STACK_SIZE as u32,
                                arg, PRIORITY, std::ptr::null_mut(), NO_AFFINITY)
    } == PD_PASS;
    if !created {
        drop(unsafe { Box::from_raw(arg as *mut Job) });
    }
    created
}

#[cfg(not(target_os = "espidf"))]
fn start_task(job: Job) -> bool {
    std::thread::Builder::new()
        .name("tailnet".to_string())
        .stack_size(STACK_SIZE)
        .spawn(move || run(job))
        .is_ok()
}

fn run(job: Job) {
    match job {
        Job::Start(on) => start_client(on),
        Job::Stop => stop_client(),
        Job::Restart(on) => {
            stop_client();
            start_client(on);
        }
        Job::Forget => {
            stop_client();
            let ret = platform::forget();
            if ret == ESP_OK {
                log!(ESP_LOG_INFO, b"this device's tailnet identity is erased\0");
            } else {
                log!(ESP_LOG_WARN, b"could not erase the tailnet identity (0x%x)\0", ret);
            }
        }
    }
    with(|s| s.busy = false);
    poke();
}

fn stop_client() {
    let ret = platform::stop();
    if ret != ESP_OK {
        // The platform keeps the client and stops it again before the next
        // start; until its tasks have gone, that waits a backoff.
        log!(ESP_LOG_WARN, b"the client did not stop cleanly (0x%x)\0", ret);
    }
    with(|s| {
        s.running_on = None;
        s.polled = None;
        if ret != ESP_OK {
            s.failures += 1;
            s.retry_at_ms = now_ms() + policy::backoff_ms(s.failures);
        }
    });
}

fn start_client(on: Underlay) {
    // The name is read now, so a rename before this start needs no restart
    let Some(hostname) = with(|s| {
        s.restart = false;
        s.hostname.clone()
    }) else {
        return;
    };
    let c_host = CString::new(hostname).unwrap_or_default();
    let ret = platform::start(&c_host);
    with(|s| {
        if ret == ESP_OK {
            // failures stays until the client connects: one that starts and
            // then gives up again backs off further each time
            s.running_on = Some(on);
            s.refused = false;
            s.failure.clear();
        } else {
            s.failures += 1;
            s.retry_at_ms = now_ms() + policy::backoff_ms(s.failures);
            s.failure = format!("The client could not start (0x{ret:x})");
        }
    });
    if ret != ESP_OK {
        log!(ESP_LOG_WARN, b"the client could not start (0x%x)\0", ret);
    }
}

// ---------------------------------------------------------------------------
// The look, once a second
// ---------------------------------------------------------------------------

/// Look again now, rather than within the second.
fn poke() {
    crate::kworker::post(tick, std::ptr::null_mut());
}

unsafe extern "C" fn tick(_arg: *mut c_void) {
    let now = now_ms();
    let net = underlay();
    let clock = clock_known();
    let poll = platform::poll();

    let mut guard = lock();
    let Some(s) = guard.as_mut() else {
        return;
    };
    match poll {
        Poll::Busy => {}
        Poll::NotRunning => s.polled = None,
        Poll::Running(p) => s.polled = Some(p),
    }
    let client = s.client();
    let client_error = s.polled.as_ref().map_or(0, |p| p.error);
    let error_text = s.polled.as_ref().map(|p| p.error_text.clone()).unwrap_or_default();
    if client == Client::Connected {
        s.failures = 0;
    }

    // Held awake while a sign-in waits, for a while.
    if client == Client::NeedsLogin && s.running_on.is_some() {
        let since = *s.login_since_ms.get_or_insert(now);
        if s.hold.is_none() && now - since < LOGIN_HOLD_MS {
            s.hold = Some(HoldGuard::new(b"Tailscale sign-in\0"));
        } else if s.hold.is_some() && now - since >= LOGIN_HOLD_MS {
            s.hold = None;
        }
    } else {
        s.login_since_ms = None;
        s.hold = None;
    }

    if s.forget && !s.busy {
        s.forget = false;
        s.busy = true;
        drop(guard);
        spawn(Job::Forget);
        return;
    }

    let inputs = Inputs {
        enabled: s.enabled,
        underlay: net.clone(),
        clock_known: clock,
        running_on: s.running_on.clone(),
        client,
        client_error,
        busy: s.busy,
        refused: s.refused,
        now_ms: now,
        retry_at_ms: s.retry_at_ms,
    };
    let mut action = policy::decide(&inputs);
    if action == Action::None && s.restart && s.running_on.is_some() && !s.busy {
        action = Action::Restart;
    }
    let shown = policy::shown(platform::available(), &inputs, s.failures > 0);
    if shown != s.shown {
        if shown == Shown::Connected {
            // What the client's tasks and sockets cost is worth knowing.
            #[cfg(not(test))]
            let free_kb = unsafe { crate::app_manager::get_free_memory() } / 1024;
            #[cfg(test)]
            let free_kb = 0usize;
            let ip = s.polled.as_ref().map_or(0, |p| p.ip);
            let host = CString::new(s.hostname.as_str()).unwrap_or_default();
            log!(ESP_LOG_INFO, b"connected as %s, %u.%u.%u.%u; internal RAM %u KB free\0",
                 host.as_ptr(),
                 ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, free_kb as u32);
        } else {
            log!(ESP_LOG_INFO, b"%s\0", shown_words(shown).as_ptr());
        }
        s.shown = shown;
    }

    let job = match action {
        Action::None => None,
        Action::Start => net.map(Job::Start),
        Action::Restart => {
            s.restart = false;
            net.map(Job::Restart)
        }
        Action::Stop => Some(Job::Stop),
        Action::StopFailed => {
            s.failures += 1;
            s.retry_at_ms = now + policy::backoff_ms(s.failures);
            let why = error_text;
            s.failure = if client_error == policy::ERROR_NODE_KEY_EXPIRED {
                "This device's key expired; it will sign in again".to_string()
            } else if why.is_empty() {
                "The client stopped".to_string()
            } else {
                why
            };
            Some(Job::Stop)
        }
        Action::StopRefused => {
            s.failure = policy::refusal_text(&error_text);
            s.refused = true;
            let text = CString::new(s.failure.as_str()).unwrap_or_default();
            log!(ESP_LOG_WARN, b"%s\0", text.as_ptr());
            Some(Job::Stop)
        }
    };
    if let Some(job) = job {
        s.busy = true;
        drop(guard);
        spawn(job);
    }
}

fn shown_words(shown: Shown) -> &'static [u8] {
    match shown {
        Shown::Off => b"off\0",
        Shown::NoNetwork => b"waiting for a network\0",
        Shown::NoClock => b"waiting for the clock\0",
        Shown::Connecting => b"connecting\0",
        Shown::NeedsLogin => b"waiting for a sign-in\0",
        Shown::NeedsApproval => b"waiting for an admin to approve this device\0",
        Shown::Connected => b"connected\0",
        Shown::Failed => b"stopped\0",
        Shown::Unavailable => b"no client on this board\0",
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

/// Read the settings and look once a second from now on. Called once by
/// kernel_init(), after the network coordinator has started.
pub(crate) fn start() {
    let json = std::fs::read_to_string(crate::system_json::path()).ok();
    let settings = settings_in(json.as_deref());
    {
        let mut guard = lock();
        if guard.is_some() {
            return;
        }
        *guard = Some(State {
            enabled: settings.enabled,
            hostname: hostname_or_default(&settings.hostname),
            running_on: None,
            busy: false,
            refused: false,
            restart: false,
            forget: false,
            failures: 0,
            retry_at_ms: 0,
            failure: String::new(),
            login_since_ms: None,
            hold: None,
            shown: Shown::Off,
            polled: None,
        });
    }
    if !platform::available() {
        return;
    }
    if settings.enabled {
        log!(ESP_LOG_INFO, b"on\0");
    }
    crate::kworker::every(tick, std::ptr::null_mut(), Duration::from_secs(1));
}

// ---------------------------------------------------------------------------
// FFI — thistle/tailnet.h
// ---------------------------------------------------------------------------

/// tailnet_status_t
#[repr(C)]
pub struct CTailnetStatus {
    pub state: u32,
    pub enabled: bool,
    pub joined: bool,
    pub ip: [u8; 16],
    pub hostname: [u8; 64],
    pub auth_url: [u8; 192],
    pub error: [u8; 128],
    pub peers: u16,
}

/// tailnet_peer_t
#[repr(C)]
pub struct CTailnetPeer {
    pub ip: [u8; 16],
    pub name: [u8; 64],
    pub direct: bool,
}

fn copy_str(dst: &mut [u8], src: &str) {
    let n = src.len().min(dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&src.as_bytes()[..n]);
    dst[n..].fill(0);
}

fn ip_text(ip: u32) -> String {
    format!("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff)
}

#[no_mangle]
pub extern "C" fn tailnet_available() -> bool {
    platform::available()
}

#[no_mangle]
pub extern "C" fn tailnet_is_enabled() -> bool {
    with(|s| s.enabled).unwrap_or(false)
}

/// Switch the tailnet on or off, saved as thistle_os.tailnet.enabled.
/// Returns at once; the client starts or stops in the background. Switching
/// on again is a fresh start, after a refusal or failures too.
#[no_mangle]
pub extern "C" fn tailnet_set_enabled(on: bool) -> i32 {
    if !platform::available() {
        return ESP_ERR_NOT_SUPPORTED;
    }
    let changed = with(|s| {
        let changed = s.enabled != on;
        s.enabled = on;
        if on {
            s.refused = false;
            s.failures = 0;
            s.retry_at_ms = 0;
            s.failure.clear();
        }
        changed
    });
    let Some(changed) = changed else {
        return ESP_ERR_INVALID_STATE;
    };
    if changed {
        log!(ESP_LOG_INFO, b"switched %s\0", if on { b"on\0".as_ptr() } else { b"off\0".as_ptr() });
    }
    poke();
    crate::system_json::save(SECTION, "enabled", serde_json::Value::Bool(on))
}

#[no_mangle]
pub extern "C" fn tailnet_is_connected() -> bool {
    with(|s| s.running_on.is_some() && s.client() == Client::Connected).unwrap_or(false)
}

/// The peers last seen, while connected.
fn peers() -> Vec<(u32, String, bool)> {
    with(|s| match &s.polled {
        Some(p) if s.running_on.is_some() && p.client == Client::Connected => p.peers.clone(),
        _ => Vec::new(),
    })
    .unwrap_or_default()
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_status_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_get_status(out: *mut CTailnetStatus) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return ESP_ERR_INVALID_ARG;
    };
    let Some((enabled, hostname, shown, failure, polled)) = with(|s| {
        (s.enabled, s.hostname.clone(), s.shown, s.failure.clone(),
         s.polled.clone().filter(|_| s.running_on.is_some()))
    }) else {
        return ESP_ERR_INVALID_STATE;
    };
    out.state = if platform::available() { shown as u32 } else { Shown::Unavailable as u32 };
    out.enabled = enabled;
    out.joined = platform::joined();
    copy_str(&mut out.hostname, &hostname);
    let connected = shown == Shown::Connected;
    let ip = polled.as_ref().filter(|_| connected).map_or(0, |p| p.ip);
    copy_str(&mut out.ip, &if ip != 0 { ip_text(ip) } else { String::new() });
    out.peers = polled.as_ref().filter(|_| connected).map_or(0, |p| p.peers.len().min(u16::MAX as usize) as u16);
    let url = polled.as_ref().filter(|_| shown == Shown::NeedsLogin).map_or("", |p| p.auth_url.as_str());
    copy_str(&mut out.auth_url, url);
    let error = if shown != Shown::Failed {
        ""
    } else if !failure.is_empty() {
        failure.as_str()
    } else {
        polled.as_ref().map_or("", |p| p.error_text.as_str())
    };
    copy_str(&mut out.error, error);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn tailnet_peer_count() -> i32 {
    peers().len() as i32
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_peer_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_get_peer(index: i32, out: *mut CTailnetPeer) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return ESP_ERR_INVALID_ARG;
    };
    let Some((ip, name, direct)) = usize::try_from(index).ok().and_then(|i| peers().into_iter().nth(i)) else {
        return ESP_ERR_INVALID_ARG;
    };
    copy_str(&mut out.ip, &ip_text(ip));
    copy_str(&mut out.name, &name);
    out.direct = direct;
    ESP_OK
}

/// Rename this device on the tailnet, saved as thistle_os.tailnet.hostname;
/// "" or NULL for the default. A running client starts again to say so.
///
/// # Safety
/// `name` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn tailnet_set_hostname(name: *const c_char) -> i32 {
    let raw = if name.is_null() {
        ""
    } else {
        match unsafe { CStr::from_ptr(name) }.to_str() {
            Ok(s) => s,
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };
    let saved = if raw.trim().is_empty() {
        String::new()
    } else {
        match policy::hostname_from(raw) {
            Some(h) => h,
            None => return ESP_ERR_INVALID_ARG,
        }
    };
    let effective = hostname_or_default(&saved);
    let Some(()) = with(|s| {
        if s.hostname != effective {
            s.hostname = effective;
            s.restart = true;
        }
    }) else {
        return ESP_ERR_INVALID_STATE;
    };
    poke();
    crate::system_json::save(SECTION, "hostname", serde_json::Value::String(saved))
}

/// Sign out: switch the tailnet off and erase this device's identity, so
/// that it signs in afresh next time. Returns at once.
#[no_mangle]
pub extern "C" fn tailnet_forget() -> i32 {
    if !platform::available() {
        return ESP_ERR_NOT_SUPPORTED;
    }
    let Some(()) = with(|s| {
        s.enabled = false;
        s.forget = true;
        s.refused = false;
        s.failure.clear();
    }) else {
        return ESP_ERR_INVALID_STATE;
    };
    poke();
    crate::system_json::save(SECTION, "enabled", serde_json::Value::Bool(false))
}

// ---------------------------------------------------------------------------
// The Terminal's `tailscale` command (thistle_shell.rs)
// ---------------------------------------------------------------------------

pub(crate) const SHELL_USAGE: &str = "Usage: tailscale [status|peers|up|down|ping NAME [PORT]]";

fn c_text(buf: &[u8]) -> String {
    CStr::from_bytes_until_nul(buf).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default()
}

/// Run `tailscale ...`: its exit code and the lines to print.
pub(crate) fn shell(args: &[&str]) -> (i32, Vec<String>) {
    let sub = args.get(1).copied().unwrap_or("status");
    match sub {
        "status" => {
            let mut st: CTailnetStatus = unsafe { std::mem::zeroed() };
            if unsafe { tailnet_get_status(&mut st) } != ESP_OK {
                return (1, vec!["tailscale: not started".into()]);
            }
            let state = match st.state {
                0 => "off",
                1 => "waiting for a network",
                2 => "waiting for the clock",
                3 => "connecting",
                4 => "waiting for a sign-in",
                5 => "waiting for an admin's approval",
                6 => "connected",
                7 => "stopped",
                _ => "not on this board",
            };
            let mut out = vec![format!("Tailscale: {state}"), format!("Name: {}", c_text(&st.hostname))];
            let ip = c_text(&st.ip);
            if !ip.is_empty() {
                out.push(format!("IP:   {ip}"));
                out.push(format!("Peers: {}", st.peers));
            }
            let url = c_text(&st.auth_url);
            if !url.is_empty() {
                out.push(format!("Sign in at {url}"));
            }
            let error = c_text(&st.error);
            if !error.is_empty() {
                out.push(error);
            }
            (0, out)
        }
        "peers" => {
            let peers = peers();
            if peers.is_empty() {
                return (0, vec!["No peers (not connected?)".into()]);
            }
            let lines = peers
                .into_iter()
                .map(|(ip, name, direct)| {
                    format!("{:15} {}{}", ip_text(ip), name, if direct { "" } else { " (relay)" })
                })
                .collect();
            (0, lines)
        }
        "up" | "down" => {
            let ret = tailnet_set_enabled(sub == "up");
            if ret == ESP_ERR_NOT_SUPPORTED {
                return (1, vec!["tailscale: no client on this board".into()]);
            }
            (0, vec![format!("Tailscale switched {}", if sub == "up" { "on" } else { "off" })])
        }
        "ping" => {
            let Some(name) = args.get(2) else {
                return (1, vec![SHELL_USAGE.into()]);
            };
            let port = match args.get(3).map(|p| p.parse::<u16>()) {
                None => 80,
                Some(Ok(p)) if p > 0 => p,
                _ => return (1, vec![SHELL_USAGE.into()]),
            };
            match reach(name, port) {
                Ok((addr, ms)) => (0, vec![format!("{name} ({addr}) answered on port {port} in {ms} ms")]),
                Err(why) => (1, vec![format!("{name}: {why}")]),
            }
        }
        _ => (1, vec![SHELL_USAGE.into()]),
    }
}

/// Connect to `name`:`port` by TCP, and say how long it took. A name is
/// looked up in the tailnet's map, never in DNS: the Terminal runs this on
/// the UI's task, which a dead resolver would hold for seconds more.
fn reach(name: &str, port: u16) -> Result<(std::net::IpAddr, u128), String> {
    use std::net::{Ipv4Addr, SocketAddr, TcpStream};
    let ip = match name.parse::<Ipv4Addr>() {
        Ok(ip) => ip,
        Err(_) => match platform::resolve(name) {
            0 => return Err("not on the tailnet (or not connected)".to_string()),
            ip => Ipv4Addr::from(ip),
        },
    };
    let addr = SocketAddr::from((ip, port));
    let began = Instant::now();
    // A first packet to a peer starts the WireGuard handshake and is
    // dropped; TCP's second SYN, 1.5 s on, finds the session.
    match TcpStream::connect_timeout(&addr, Duration::from_secs(5)) {
        Ok(_) => Ok((addr.ip(), began.elapsed().as_millis())),
        Err(e) if e.kind() == std::io::ErrorKind::ConnectionRefused => {
            Err(format!("{} refused port {port}, so it is reachable", addr.ip()))
        }
        Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {
            Err(format!("{} did not answer on port {port} within 5 s", addr.ip()))
        }
        Err(e) => Err(format!("{}: {e}", addr.ip())),
    }
}

// ---------------------------------------------------------------------------
// The client (components/kernel/src/tailnet_platform.c)
// ---------------------------------------------------------------------------

#[cfg(target_os = "espidf")]
mod platform {
    use std::ffi::{c_char, CStr};

    #[repr(C)]
    struct Peer {
        ip: u32,
        name: [u8; 64],
        direct: bool,
    }

    /// tailnet_platform_poll_t
    #[repr(C)]
    struct PollOut {
        state: i32,
        error: i32,
        registered: bool,
        ip: u32,
        peer_count: i32,
        auth_url: [u8; 192],
        error_text: [u8; 128],
    }

    /// MicroLink's own limit (CONFIG_ML_MAX_PEERS)
    const MAX_PEERS: usize = 16;
    const POLL_RUNNING: i32 = 0;
    const POLL_BUSY: i32 = -2;

    extern "C" {
        fn tailnet_platform_available() -> bool;
        fn tailnet_platform_default_name(buf: *mut c_char, len: usize);
        fn tailnet_platform_start(hostname: *const c_char) -> i32;
        fn tailnet_platform_stop() -> i32;
        fn tailnet_platform_poll(out: *mut PollOut, peers: *mut Peer, max_peers: i32) -> i32;
        fn tailnet_platform_joined() -> bool;
        fn tailnet_platform_resolve(name: *const c_char) -> u32;
        fn tailnet_platform_forget() -> i32;
    }

    fn text(buf: &[u8]) -> String {
        CStr::from_bytes_until_nul(buf).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default()
    }

    pub(super) fn available() -> bool {
        unsafe { tailnet_platform_available() }
    }
    pub(super) fn default_name(buf: &mut [u8]) {
        unsafe { tailnet_platform_default_name(buf.as_mut_ptr() as *mut c_char, buf.len()) }
    }
    pub(super) fn start(hostname: &CStr) -> i32 {
        unsafe { tailnet_platform_start(hostname.as_ptr()) }
    }
    pub(super) fn stop() -> i32 {
        unsafe { tailnet_platform_stop() }
    }
    pub(super) fn poll() -> super::Poll {
        // About 1.2 KB, on the kernel worker's 16 KB stack
        let mut peers: [Peer; MAX_PEERS] = std::array::from_fn(|_| Peer { ip: 0, name: [0; 64], direct: false });
        let mut out = PollOut { state: 0, error: 0, registered: false, ip: 0, peer_count: 0,
                                auth_url: [0; 192], error_text: [0; 128] };
        match unsafe { tailnet_platform_poll(&mut out, peers.as_mut_ptr(), MAX_PEERS as i32) } {
            POLL_RUNNING => {}
            POLL_BUSY => return super::Poll::Busy,
            _ => return super::Poll::NotRunning,
        }
        let n = (out.peer_count.max(0) as usize).min(MAX_PEERS);
        super::Poll::Running(super::Polled {
            client: super::Client::from_raw(out.state),
            error: out.error,
            error_text: text(&out.error_text),
            ip: out.ip,
            auth_url: text(&out.auth_url),
            peers: peers.iter().take(n).filter(|p| p.ip != 0).map(|p| (p.ip, text(&p.name), p.direct)).collect(),
        })
    }
    pub(super) fn joined() -> bool {
        unsafe { tailnet_platform_joined() }
    }
    pub(super) fn resolve(name: &str) -> u32 {
        let Ok(name) = std::ffi::CString::new(name) else {
            return 0;
        };
        unsafe { tailnet_platform_resolve(name.as_ptr()) }
    }
    pub(super) fn forget() -> i32 {
        unsafe { tailnet_platform_forget() }
    }
}

/// Off the device: a pretend client, for the simulators and the tests. The
/// host is often on a tailnet already, so sockets reach real peers anyway.
///
/// THISTLE_SIM_TAILNET says how the control plane behaves with a device
/// that has not joined:
/// - `login` (the default): a URL to sign in at, signed in
///   THISTLE_SIM_TAILNET_LOGIN_S seconds later (5; `never` for never);
/// - `approval`: as `login`, then waiting as long again for an admin;
/// - `refuse`: the registration is refused.
///
/// The peers are two made up, or the host's own with
/// THISTLE_SIM_TAILNET_PEERS=host, from `tailscale status --json`. Whether
/// the device has joined is a file in the simulator's SPIFFS, so that it
/// survives a restart.
#[cfg(not(target_os = "espidf"))]
mod platform {
    use std::ffi::CStr;
    use std::path::PathBuf;
    use std::sync::Mutex;
    use std::time::Instant;

    struct Fake {
        started: Instant,
        joined_before: bool,
    }

    static FAKE: Mutex<Option<Fake>> = Mutex::new(None);

    pub(super) const REFUSAL: &str = "node not allowed by the tailnet's policy";
    const URL: &str = "https://login.tailscale.com/a/thistlesim0";

    fn env(name: &str) -> String {
        std::env::var(name).unwrap_or_default()
    }

    fn joined_file() -> PathBuf {
        let spiffs = std::env::var("THISTLE_SIM_SPIFFS")
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "/tmp/thistle_spiffs".to_string());
        PathBuf::from(spiffs).join(".tailnet_joined")
    }

    fn login_s() -> Option<u64> {
        let v = env("THISTLE_SIM_TAILNET_LOGIN_S");
        if v == "never" {
            return None;
        }
        Some(v.parse().unwrap_or(5))
    }

    /// (state, registered, error) at `t` seconds after starting.
    fn at(f: &Fake, t: f64) -> (i32, bool, i32) {
        const CONNECTING: i32 = 2;
        const CONNECTED: i32 = 4;
        const ERROR: i32 = 6;
        const NEEDS_LOGIN: i32 = 7;
        const NEEDS_APPROVAL: i32 = 8;
        if t < 1.0 {
            return (CONNECTING, false, 0);
        }
        let mode = env("THISTLE_SIM_TAILNET");
        if f.joined_before {
            return if t < 2.0 { (CONNECTING, true, 0) } else { (CONNECTED, true, 0) };
        }
        if mode == "refuse" {
            return (ERROR, false, super::policy::ERROR_REFUSED);
        }
        let Some(wait) = login_s().map(|s| s as f64) else {
            return (NEEDS_LOGIN, false, 0);
        };
        if t < 1.0 + wait {
            return (NEEDS_LOGIN, false, 0);
        }
        let approved_at = if mode == "approval" { 1.0 + 2.0 * wait } else { 1.0 + wait };
        if t < approved_at {
            return (NEEDS_APPROVAL, false, 0);
        }
        if t < approved_at + 1.0 {
            return (CONNECTING, true, 0);
        }
        (CONNECTED, true, 0)
    }

    fn now() -> Option<(i32, bool, i32)> {
        let guard = FAKE.lock().ok()?;
        let f = guard.as_ref()?;
        let now = at(f, f.started.elapsed().as_secs_f64());
        if now.1 && !f.joined_before {
            let _ = std::fs::create_dir_all(joined_file().parent()?);
            let _ = std::fs::write(joined_file(), b"1");
        }
        Some(now)
    }

    pub(super) fn available() -> bool {
        true
    }
    pub(super) fn default_name(buf: &mut [u8]) {
        let name = b"thistle-sim\0";
        let n = name.len().min(buf.len());
        buf[..n].copy_from_slice(&name[..n]);
    }
    pub(super) fn start(_hostname: &CStr) -> i32 {
        let Ok(mut guard) = FAKE.lock() else {
            return super::ESP_ERR_INVALID_STATE;
        };
        if guard.is_some() {
            return super::ESP_ERR_INVALID_STATE;
        }
        *guard = Some(Fake {
            started: Instant::now(),
            joined_before: joined_file().is_file(),
        });
        super::ESP_OK
    }
    pub(super) fn stop() -> i32 {
        if let Ok(mut guard) = FAKE.lock() {
            *guard = None;
        }
        super::ESP_OK
    }
    pub(super) fn poll() -> super::Poll {
        let Some((state, _registered, error)) = now() else {
            return super::Poll::NotRunning;
        };
        let connected = state == 4;
        super::Poll::Running(super::Polled {
            client: super::Client::from_raw(state),
            error,
            error_text: if error != 0 { REFUSAL.to_string() } else { String::new() },
            ip: if connected { own_ip() } else { 0 },
            auth_url: if state == 7 { URL.to_string() } else { String::new() },
            peers: if connected { peers() } else { Vec::new() },
        })
    }
    pub(super) fn joined() -> bool {
        joined_file().is_file()
    }
    pub(super) fn resolve(name: &str) -> u32 {
        let want = name.to_ascii_lowercase();
        peers()
            .into_iter()
            .find(|(_, n, _)| {
                let n = n.to_ascii_lowercase();
                n == want || n.split('.').next() == Some(want.as_str())
            })
            .map_or(0, |(ip, _, _)| ip)
    }

    fn peers() -> Vec<(u32, String, bool)> {
        if env("THISTLE_SIM_TAILNET_PEERS") == "host" {
            return host_peers().map(|(_, p)| p).unwrap_or_default();
        }
        vec![
            (0x6440_0002, "homelab.tail-sim.ts.net".to_string(), true),
            (0x6440_0003, "laptop.tail-sim.ts.net".to_string(), false),
        ]
    }

    fn parse_ip(s: &str) -> Option<u32> {
        let o: Vec<u32> = s.split('.').map(|p| p.parse().ok()).collect::<Option<_>>()?;
        (o.len() == 4 && o.iter().all(|&b| b < 256))
            .then(|| (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3])
    }

    /// The host's own tailnet address and peers.
    fn host_peers() -> Option<(u32, Vec<(u32, String, bool)>)> {
        let out = std::process::Command::new("tailscale").args(["status", "--json"]).output().ok()?;
        let v: serde_json::Value = serde_json::from_slice(&out.stdout).ok()?;
        let ipv4 = |node: &serde_json::Value| {
            node.get("TailscaleIPs")?.as_array()?.iter().filter_map(|i| i.as_str()).find_map(parse_ip)
        };
        let me = ipv4(v.get("Self")?)?;
        let peers = v
            .get("Peer")
            .and_then(|p| p.as_object())
            .map(|m| {
                m.values()
                    .filter_map(|p| {
                        let name = p.get("DNSName")?.as_str()?.trim_end_matches('.').to_string();
                        let direct = p.get("CurAddr").and_then(|a| a.as_str()).is_some_and(|a| !a.is_empty());
                        Some((ipv4(p)?, name, direct))
                    })
                    .collect()
            })
            .unwrap_or_default();
        Some((me, peers))
    }

    fn own_ip() -> u32 {
        if env("THISTLE_SIM_TAILNET_PEERS") == "host" {
            if let Some((me, _)) = host_peers() {
                return me;
            }
        }
        0x6440_0001
    }
    pub(super) fn forget() -> i32 {
        let _ = std::fs::remove_file(joined_file());
        super::ESP_OK
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn off_unless_switched_on() {
        assert_eq!(settings_in(None), Settings { enabled: false, hostname: String::new() });
        assert_eq!(settings_in(Some("not json")).enabled, false);
        let on = settings_in(Some(r#"{"thistle_os": {"tailnet": {"enabled": true}}}"#));
        assert!(on.enabled);
    }

    #[test]
    fn a_saved_name_is_made_one_the_tailnet_takes() {
        let s = settings_in(Some(r#"{"thistle_os": {"tailnet": {"hostname": "My Deck"}}}"#));
        assert_eq!(s.hostname, "my-deck");
        let blank = settings_in(Some(r#"{"thistle_os": {"tailnet": {"hostname": "!!"}}}"#));
        assert_eq!(blank.hostname, "");
        assert_eq!(hostname_or_default(""), "thistle-sim");
        assert_eq!(hostname_or_default("mine"), "mine");
    }

    #[test]
    fn copies_a_string_into_a_c_buffer() {
        let mut buf = [0xffu8; 8];
        copy_str(&mut buf, "abc");
        assert_eq!(&buf, b"abc\0\0\0\0\0");
        copy_str(&mut buf, "0123456789");
        assert_eq!(&buf, b"0123456\0");
        assert_eq!(ip_text(0x6440_0001), "100.64.0.1");
    }
}
