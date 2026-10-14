// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Kernel — mesh_manager module
//
// The kernel's side of the MeshCore mesh. The engine (components/meshcore)
// runs the node on the kernel worker and hears everything; this owns what apps see
// of it: who this node is, the contacts it has heard from, messages waiting for
// an app to read them, and what became of each message an app sent.
//
// It also decides what the engine is given. A MeshCore node only hears peers
// on the same frequency, bandwidth and spreading factor, so the board's band
// picks MeshCore's own preset for that region, and system.json's `mesh`
// section can override any of it:
//
//   "mesh": { "enabled": true, "name": "Mo's T-Deck",
//             "frequency_mhz": 869.618, "bandwidth_khz": 62.5,
//             "spreading_factor": 8, "coding_rate": 5, "tx_power_dbm": 22 }
//
// Every message received or sent is also recorded in the message history
// (msg_log), whether or not an app is open to read it.
//
// On simulator and test builds there is no engine: the mesh never starts, and
// the store below is exercised directly.

use std::collections::VecDeque;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicU32, AtomicU8, Ordering};
use std::sync::Mutex;

// ---------------------------------------------------------------------------
// ESP-IDF error codes
// ---------------------------------------------------------------------------

const ESP_OK: i32 = 0x000;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;
const ESP_FAIL: i32 = -1;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const INBOX_SIZE: usize = 32;
const DELIVERY_QUEUE_SIZE: usize = 32;
/// How many recent messages a repeat is checked against. A sender that hears
/// no acknowledgement sends the same message again, and it can arrive twice.
const RECENT_SIGNATURES: usize = 32;
const MESH_NAME_MAX: usize = 32;
/// MeshCore's MAX_TEXT_LEN: ten cipher blocks.
pub const MESH_TEXT_MAX: usize = 160;
/// The channel every MeshCore node shares.
pub const PUBLIC_CHANNEL: u8 = 0;

/// What a contact's path length is when no route to it is known.
pub const PATH_UNKNOWN: u8 = 0xFF;

pub const MSG_DIRECT: u8 = 0;
pub const MSG_CHANNEL: u8 = 1;

/// Delivery states, as the engine reports them.
pub const DELIVERY_SENT: u8 = 1;
pub const DELIVERY_DELIVERED: u8 = 2;
pub const DELIVERY_FAILED: u8 = 3;

/// Where the engine keeps the node's identity and contacts.
#[cfg(target_os = "espidf")]
const STATE_DIR: &str = "/spiffs/config";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

macro_rules! mesh_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        #[cfg(not(test))]
        unsafe {
            esp_log_write($level, b"mesh\0".as_ptr(), $fmt.as_ptr() $(, $arg)*);
        }
        #[cfg(test)]
        {
            let _ = ($level, $fmt $(, $arg)*);
        }
    }};
}

// ---------------------------------------------------------------------------
// FFI structs — C-compatible, shared with apps (thistle_app.h)
// ---------------------------------------------------------------------------

/// A contact this node has heard an advert from.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CMeshContact {
    pub pub_key: [u8; 32],
    pub name: [u8; 32],
    pub name_len: u8,
    pub node_type: u8,
    pub last_rssi: i8,
    /// Hops to it, or PATH_UNKNOWN when a message to it has to flood.
    pub path_len: u8,
    /// When it last advertised, by its own clock.
    pub last_seen: u32,
    pub lat: f64,
    pub lon: f64,
    pub has_position: bool,
}

impl Default for CMeshContact {
    fn default() -> Self {
        Self {
            pub_key: [0u8; 32],
            name: [0u8; 32],
            name_len: 0,
            node_type: 0,
            last_rssi: 0,
            path_len: PATH_UNKNOWN,
            last_seen: 0,
            lat: 0.0,
            lon: 0.0,
            has_position: false,
        }
    }
}

/// A received message waiting for an app.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CMeshMessage {
    /// The sender, for a direct message; zero for a channel message, whose
    /// sender is known only by the name it gave.
    pub sender_key: [u8; 32],
    pub sender_name: [u8; 32],
    pub sender_name_len: u8,
    /// When it was sent, by the sender's clock.
    pub timestamp: u32,
    pub text: [u8; 200],
    pub text_len: u16,
    /// MSG_DIRECT or MSG_CHANNEL.
    pub kind: u8,
    /// The channel, for a channel message.
    pub channel: u8,
}

impl Default for CMeshMessage {
    fn default() -> Self {
        Self {
            sender_key: [0u8; 32],
            sender_name: [0u8; 32],
            sender_name_len: 0,
            timestamp: 0,
            text: [0u8; 200],
            text_len: 0,
            kind: MSG_DIRECT,
            channel: 0,
        }
    }
}

/// Network statistics from the mesh layer.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CMeshStats {
    pub packets_sent: u32,
    pub packets_received: u32,
    pub packets_forwarded: u32,
    pub messages_sent: u32,
    pub messages_received: u32,
    pub contacts_discovered: u32,
}

// ---------------------------------------------------------------------------
// The engine's C interface (components/meshcore/include/meshcore_shim.h)
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
struct EngineConfig {
    freq_hz: u32,
    bw_hz: u32,
    sf: u8,
    cr: u8,
    tx_power_dbm: i8,
    name: [u8; 32],
    state_dir: [u8; 48],
}

#[repr(C)]
#[derive(Clone, Copy)]
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
pub(crate) struct EngineContact {
    pub_key: [u8; 32],
    name: [u8; 32],
    node_type: u8,
    path_len: u8,
    last_advert: u32,
    lat_e6: i32,
    lon_e6: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
pub(crate) struct EngineMessage {
    kind: u8,
    channel: u8,
    sender_key: [u8; 32],
    sender_name: [u8; 32],
    timestamp: u32,
    text: [u8; MESH_TEXT_MAX + 1],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct EngineStats {
    packets_sent: u32,
    packets_received: u32,
    messages_sent: u32,
    messages_received: u32,
    rx_dropped: u32,
}

#[repr(C)]
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
struct EngineCallbacks {
    on_ready: unsafe extern "C" fn(*const u8, *const c_char),
    on_contact: unsafe extern "C" fn(*const EngineContact),
    on_contact_removed: unsafe extern "C" fn(*const u8),
    on_message: unsafe extern "C" fn(*const EngineMessage),
    on_delivery: unsafe extern "C" fn(u32, u8),
}

#[cfg(target_os = "espidf")]
extern "C" {
    fn meshcore_start(config: *const EngineConfig, callbacks: *const EngineCallbacks) -> i32;
    fn meshcore_stop() -> i32;
    fn meshcore_send_direct(pub_key: *const u8, text: *const c_char, message_id: u32) -> i32;
    fn meshcore_send_channel(channel: u8, text: *const c_char, message_id: u32) -> i32;
    fn meshcore_send_advert() -> i32;
    fn meshcore_set_name(name: *const c_char) -> i32;
    fn meshcore_get_stats(out: *mut EngineStats) -> i32;
    fn meshcore_outbound_pending() -> i32;
}

/// Whether the engine has packets queued to go out, an acknowledgement or a
/// relay. The power manager keeps the CPU up for them after a packet wakes
/// it. Call from the kernel worker, where the engine runs.
pub(crate) fn outbound_pending() -> bool {
    #[cfg(target_os = "espidf")]
    {
        unsafe { meshcore_outbound_pending() > 0 }
    }
    #[cfg(not(target_os = "espidf"))]
    {
        false
    }
}

// ---------------------------------------------------------------------------
// Radio settings
// ---------------------------------------------------------------------------

/// What the engine tunes the radio to.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct MeshParams {
    pub freq_hz: u32,
    pub bw_hz: u32,
    pub sf: u8,
    pub cr: u8,
    pub tx_power_dbm: i8,
}

/// MeshCore's own default, and its "EU/UK (Narrow)" preset (platformio.ini
/// in the MeshCore tree): inside the 869.4–869.65 MHz sub-band.
const EU_PRESET: MeshParams = MeshParams {
    freq_hz: 869_618_000,
    bw_hz: 62_500,
    sf: 8,
    cr: 5,
    tx_power_dbm: 22,
};

/// MeshCore's "USA/Canada (Recommended)" preset (docs/faq.md, 2.3).
const US_PRESET: MeshParams = MeshParams {
    freq_hz: 910_525_000,
    bw_hz: 62_500,
    sf: 7,
    cr: 5,
    tx_power_dbm: 22,
};

/// A band a board's radio is matched for, and where in it a node may sit.
struct Band {
    low_hz: u32,
    high_hz: u32,
    preset: MeshParams,
}

const BANDS: [Band; 2] = [
    Band {
        low_hz: 863_000_000,
        high_hz: 870_000_000,
        preset: EU_PRESET,
    },
    Band {
        low_hz: 902_000_000,
        high_hz: 928_000_000,
        preset: US_PRESET,
    },
];

fn band_containing(hz: u32) -> Option<&'static Band> {
    BANDS.iter().find(|b| (b.low_hz..=b.high_hz).contains(&hz))
}

/// The radio settings for a board whose radio is matched for `band_hz`, with
/// whatever the `mesh` section of system.json overrides. `None` when there is
/// nothing to go on: no radio, or a band MeshCore has no preset for and no
/// frequency given.
///
/// A frequency outside the board's band is refused rather than obeyed: the
/// module is matched for one band, and the other is not licensed where the
/// board was sold.
pub(crate) fn mesh_params(band_hz: Option<u32>, json: Option<&str>) -> Option<MeshParams> {
    let band = band_hz?;
    let setting = |key: &str| json.and_then(|j| crate::system_json::get(j, "mesh", key));
    let number = |key: &str| setting(key).and_then(|v| v.as_f64());

    let board_band = band_containing(band);
    let requested_hz = number("frequency_mhz")
        .filter(|mhz| mhz.is_finite() && *mhz > 0.0)
        .map(|mhz| (mhz * 1_000_000.0).round() as u32);

    let mut params = match (board_band, requested_hz) {
        (Some(b), Some(hz)) if (b.low_hz..=b.high_hz).contains(&hz) => MeshParams {
            freq_hz: hz,
            ..b.preset
        },
        (Some(b), Some(hz)) => {
            mesh_log!(
                ESP_LOG_WARN,
                b"mesh: %u Hz is outside this radio's band; using %u Hz\n\0",
                hz,
                b.preset.freq_hz
            );
            b.preset
        }
        (Some(b), None) => b.preset,
        (None, Some(hz)) => MeshParams {
            freq_hz: hz,
            ..EU_PRESET
        },
        (None, None) => return None,
    };

    if let Some(khz) = number("bandwidth_khz").filter(|k| k.is_finite() && *k > 0.0) {
        params.bw_hz = (khz * 1000.0).round() as u32;
    }
    if let Some(sf) = number("spreading_factor").filter(|sf| (5.0..=12.0).contains(sf)) {
        params.sf = sf as u8;
    }
    if let Some(cr) = number("coding_rate").filter(|cr| (5.0..=8.0).contains(cr)) {
        params.cr = cr as u8;
    }
    if let Some(dbm) = number("tx_power_dbm").filter(|d| (-9.0..=22.0).contains(d)) {
        params.tx_power_dbm = dbm as i8;
    }
    Some(params)
}

/// Whether system.json leaves the mesh on. It is on unless switched off.
pub(crate) fn mesh_enabled(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, "mesh", "enabled"))
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}

/// The name system.json gives this node, if any, cut to what fits an advert.
pub(crate) fn configured_name(json: Option<&str>) -> Option<String> {
    let name = json.and_then(|j| crate::system_json::get(j, "mesh", "name"))?;
    let name = name.as_str()?.trim();
    if name.is_empty() {
        return None;
    }
    Some(truncate_utf8(name, MESH_NAME_MAX - 1).to_string())
}

fn truncate_utf8(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

/// A message's identity for spotting repeats: who, when, and what.
#[derive(Clone, Copy, PartialEq)]
struct Signature {
    kind: u8,
    channel: u8,
    sender: [u8; 32],
    timestamp: u32,
    text_hash: u64,
}

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, b| {
        (h ^ *b as u64).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

struct MeshState {
    running: bool,
    self_key: [u8; 32],
    self_name: [u8; MESH_NAME_MAX],
    /// Most recently heard first.
    contacts: Vec<CMeshContact>,
    inbox: VecDeque<CMeshMessage>,
    /// Messages an app has taken from the inbox and not yet shown the user,
    /// as it last reported.
    held_unread: u32,
    recent: VecDeque<Signature>,
    deliveries: VecDeque<(u32, u8)>,
}

impl MeshState {
    const fn new() -> Self {
        Self {
            running: false,
            self_key: [0u8; 32],
            self_name: [0u8; MESH_NAME_MAX],
            contacts: Vec::new(),
            inbox: VecDeque::new(),
            held_unread: 0,
            recent: VecDeque::new(),
            deliveries: VecDeque::new(),
        }
    }

    fn upsert_contact(&mut self, contact: CMeshContact) {
        self.contacts.retain(|c| c.pub_key != contact.pub_key);
        let at = self
            .contacts
            .iter()
            .position(|c| c.last_seen < contact.last_seen)
            .unwrap_or(self.contacts.len());
        self.contacts.insert(at, contact);
    }

    fn remove_contact(&mut self, pub_key: &[u8; 32]) {
        self.contacts.retain(|c| &c.pub_key != pub_key);
    }

    /// Queue a message for apps, unless it is a repeat of one already
    /// queued. Returns whether it was new.
    fn receive(&mut self, msg: CMeshMessage) -> bool {
        let sig = Signature {
            kind: msg.kind,
            channel: msg.channel,
            sender: if msg.kind == MSG_DIRECT {
                msg.sender_key
            } else {
                let mut name = [0u8; 32];
                name.copy_from_slice(&msg.sender_name);
                name
            },
            timestamp: msg.timestamp,
            text_hash: fnv1a(&msg.text[..msg.text_len as usize]),
        };
        if self.recent.contains(&sig) {
            return false;
        }
        if self.recent.len() == RECENT_SIGNATURES {
            self.recent.pop_front();
        }
        self.recent.push_back(sig);

        if self.inbox.len() == INBOX_SIZE {
            self.inbox.pop_front();
        }
        self.inbox.push_back(msg);
        true
    }

    fn deliver(&mut self, id: u32, state: u8) {
        if self.deliveries.len() == DELIVERY_QUEUE_SIZE {
            self.deliveries.pop_front();
        }
        self.deliveries.push_back((id, state));
    }
}

static MESH_STATE: Mutex<MeshState> = Mutex::new(MeshState::new());
static RADIO_BAND_HZ: Mutex<Option<u32>> = Mutex::new(None);
/// What the engine was started on, for anyone setting another node to match.
static PARAMS_IN_USE: Mutex<Option<MeshParams>> = Mutex::new(None);
static NEXT_MESSAGE_ID: AtomicU32 = AtomicU32::new(1);
/// The name as a C string for rs_mesh_get_self_name(): its address must stay
/// put after the call returns, so it lives in a static of its own.
static SELF_NAME_C: Mutex<[u8; MESH_NAME_MAX]> = Mutex::new([0u8; MESH_NAME_MAX]);

fn next_message_id() -> u32 {
    loop {
        let id = NEXT_MESSAGE_ID.fetch_add(1, Ordering::Relaxed);
        if id != 0 {
            return id;
        }
    }
}

fn copy_name(dest: &mut [u8; MESH_NAME_MAX], src: &[u8]) -> u8 {
    let src = &src[..src.iter().position(|b| *b == 0).unwrap_or(src.len())];
    let n = src.len().min(MESH_NAME_MAX - 1);
    *dest = [0u8; MESH_NAME_MAX];
    dest[..n].copy_from_slice(&src[..n]);
    n as u8
}

fn set_self_name(state: &mut MeshState, name: &[u8]) {
    copy_name(&mut state.self_name, name);
    if let Ok(mut c) = SELF_NAME_C.lock() {
        *c = state.self_name;
    }
}

/// Copy a C string into a buffer of `N` bytes, NUL-terminated, cut short if
/// it has to be.
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
fn to_c_array<const N: usize>(s: &str) -> [u8; N] {
    let mut out = [0u8; N];
    let s = truncate_utf8(s, N - 1);
    out[..s.len()].copy_from_slice(s.as_bytes());
    out
}

// ---------------------------------------------------------------------------
// Conversions from the engine
// ---------------------------------------------------------------------------

pub(crate) fn contact_from_engine(c: &EngineContact) -> CMeshContact {
    let mut out = CMeshContact {
        pub_key: c.pub_key,
        node_type: c.node_type,
        path_len: c.path_len,
        last_seen: c.last_advert,
        ..Default::default()
    };
    out.name_len = copy_name(&mut out.name, &c.name);
    if c.lat_e6 != 0 || c.lon_e6 != 0 {
        out.lat = c.lat_e6 as f64 / 1_000_000.0;
        out.lon = c.lon_e6 as f64 / 1_000_000.0;
        out.has_position = true;
    }
    out
}

pub(crate) fn message_from_engine(m: &EngineMessage) -> CMeshMessage {
    let mut out = CMeshMessage {
        sender_key: m.sender_key,
        timestamp: m.timestamp,
        kind: m.kind,
        channel: m.channel,
        ..Default::default()
    };
    out.sender_name_len = copy_name(&mut out.sender_name, &m.sender_name);
    let text = &m.text[..m.text.iter().position(|b| *b == 0).unwrap_or(m.text.len())];
    let n = text.len().min(out.text.len() - 1);
    out.text[..n].copy_from_slice(&text[..n]);
    out.text_len = n as u16;
    out
}

// ---------------------------------------------------------------------------
// Callbacks from the engine, on the kernel worker
// ---------------------------------------------------------------------------

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
unsafe extern "C" fn on_ready(pub_key: *const u8, name: *const c_char) {
    if pub_key.is_null() || name.is_null() {
        return;
    }
    let name = std::ffi::CStr::from_ptr(name).to_bytes();
    if let Ok(mut state) = MESH_STATE.lock() {
        state.self_key.copy_from_slice(std::slice::from_raw_parts(pub_key, 32));
        set_self_name(&mut state, name);
        state.running = true;
    }
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
unsafe extern "C" fn on_contact(contact: *const EngineContact) {
    if contact.is_null() {
        return;
    }
    let contact = contact_from_engine(&*contact);
    if let Ok(mut state) = MESH_STATE.lock() {
        state.upsert_contact(contact);
    }
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
unsafe extern "C" fn on_contact_removed(pub_key: *const u8) {
    if pub_key.is_null() {
        return;
    }
    let key = &*(pub_key as *const [u8; 32]);
    if let Ok(mut state) = MESH_STATE.lock() {
        state.remove_contact(key);
    }
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
unsafe extern "C" fn on_message(message: *const EngineMessage) {
    if message.is_null() {
        return;
    }
    hear(message_from_engine(&*message));
}

/// Queue a message for apps and record it, unless it is a repeat. Returns
/// whether it was new.
fn hear(message: CMeshMessage) -> bool {
    let new = match MESH_STATE.lock() {
        Ok(mut state) => state.receive(message),
        Err(_) => false,
    };
    if new {
        record_received(&message);
        alert_received(&message);
    }
    new
}

// ---------------------------------------------------------------------------
// The message history
// ---------------------------------------------------------------------------

/// A conversation's name in the history: a channel, or a contact by the
/// first MSG_MESH_KEY_PREFIX bytes of its key, as the Messenger addresses it.
fn history_conv(kind: u8, channel: u8, key: &[u8; 32]) -> String {
    if kind == MSG_CHANNEL {
        if channel == PUBLIC_CHANNEL {
            "public".to_string()
        } else {
            format!("channel{channel}")
        }
    } else {
        key[..6].iter().map(|b| format!("{b:02x}")).collect()
    }
}

/// Buzz and pop up for a message heard, unless the Messenger shows its
/// conversation: a channel's is the Public one, "" there, and a contact's is
/// its address, as history_conv() makes it.
fn alert_received(m: &CMeshMessage) {
    let name = String::from_utf8_lossy(&m.sender_name[..(m.sender_name_len as usize).min(32)]);
    let name = if name.is_empty() { "?".into() } else { name };
    let text = String::from_utf8_lossy(&m.text[..(m.text_len as usize).min(m.text.len())]);
    let (conv, from) = if m.kind == MSG_CHANNEL {
        let channel = if m.channel == PUBLIC_CHANNEL {
            "Public".to_string()
        } else {
            format!("channel {}", m.channel)
        };
        (String::new(), format!("{name} in {channel}"))
    } else {
        (history_conv(m.kind, m.channel, &m.sender_key), name.into_owned())
    };
    crate::msg_alert::arrived(crate::msg_alert::Transport::Mesh, &conv, &from, &text);
}

fn record_received(m: &CMeshMessage) {
    let name = String::from_utf8_lossy(&m.sender_name[..(m.sender_name_len as usize).min(32)]);
    let text = String::from_utf8_lossy(&m.text[..(m.text_len as usize).min(m.text.len())]);
    crate::msg_log::record(&crate::msg_log::Entry {
        transport: crate::msg_log::Transport::Mesh,
        conv: &history_conv(m.kind, m.channel, &m.sender_key),
        dir: crate::msg_log::Direction::In,
        peer: &name,
        text: &text,
        sent: (m.timestamp != 0).then_some(m.timestamp as u64),
    });
}

fn record_sent(kind: u8, channel: u8, key: &[u8; 32], to: &str, text: *const c_char) {
    let text = unsafe { std::ffi::CStr::from_ptr(text) }.to_string_lossy();
    crate::msg_log::record(&crate::msg_log::Entry {
        transport: crate::msg_log::Transport::Mesh,
        conv: &history_conv(kind, channel, key),
        dir: crate::msg_log::Direction::Out,
        peer: to,
        text: &text,
        sent: None,
    });
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
unsafe extern "C" fn on_delivery(id: u32, delivery: u8) {
    if let Ok(mut state) = MESH_STATE.lock() {
        state.deliver(id, delivery);
    }
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
static CALLBACKS: EngineCallbacks = EngineCallbacks {
    on_ready,
    on_contact,
    on_contact_removed,
    on_message,
    on_delivery,
};

// ---------------------------------------------------------------------------
// Starting the mesh
// ---------------------------------------------------------------------------

/// Record the band the board's radio is matched for. board_config calls this
/// once the drivers are loaded.
pub(crate) fn declare_radio_band(band_hz: Option<u32>) {
    if let Ok(mut band) = RADIO_BAND_HZ.lock() {
        *band = band_hz;
    }
}

fn radio_band() -> Option<u32> {
    RADIO_BAND_HZ.lock().ok().and_then(|b| *b)
}

fn read_system_json() -> Option<String> {
    std::fs::read_to_string(crate::system_json::path()).ok()
}

// ---------------------------------------------------------------------------
// Switched on or off
// ---------------------------------------------------------------------------

const UNKNOWN: u8 = 0;
const ON: u8 = 1;
const OFF: u8 = 2;

/// `mesh.enabled`, read from system.json once and kept here after.
static ENABLED: AtomicU8 = AtomicU8::new(UNKNOWN);

fn enabled() -> bool {
    match ENABLED.load(Ordering::Relaxed) {
        ON => true,
        OFF => false,
        _ => {
            let on = mesh_enabled(read_system_json().as_deref());
            ENABLED.store(if on { ON } else { OFF }, Ordering::Relaxed);
            on
        }
    }
}

/// Put the radio to sleep, where it hears nothing and draws next to nothing.
fn radio_sleep() {
    let reg = crate::hal_registry::registry();
    if let Some(radio) = unsafe { reg.radio.as_ref() } {
        unsafe {
            if let Some(stop) = radio.stop_receive {
                stop();
            }
            if let Some(sleep) = radio.sleep {
                sleep(true);
            }
        }
    }
}

/// Take the node off air and put the radio to sleep.
fn mesh_stop() -> i32 {
    #[cfg(target_os = "espidf")]
    let rc = unsafe { meshcore_stop() };
    #[cfg(not(target_os = "espidf"))]
    let rc = {
        radio_sleep();
        ESP_OK
    };
    if rc == ESP_OK {
        if let Ok(mut state) = MESH_STATE.lock() {
            state.running = false;
        }
    }
    rc
}

/// Bring the mesh up, if the board has a radio and system.json has not
/// switched it off. kernel_init() calls this once the drivers have started and
/// the antenna the user chose is selected.
pub(crate) fn mesh_start() -> i32 {
    if rs_mesh_is_running() {
        return ESP_OK;
    }
    let json = read_system_json();
    if !enabled() {
        mesh_log!(ESP_LOG_INFO, b"mesh: off in system.json; radio asleep\n\0");
        radio_sleep();
        return ESP_ERR_NOT_SUPPORTED;
    }
    let params = match mesh_params(radio_band(), json.as_deref()) {
        Some(p) => p,
        None => return ESP_ERR_NOT_FOUND,
    };
    #[cfg(target_os = "espidf")]
    {
        let name = configured_name(json.as_deref()).unwrap_or_default();
        let config = EngineConfig {
            freq_hz: params.freq_hz,
            bw_hz: params.bw_hz,
            sf: params.sf,
            cr: params.cr,
            tx_power_dbm: params.tx_power_dbm,
            name: to_c_array(&name),
            state_dir: to_c_array(STATE_DIR),
        };
        let rc = unsafe { meshcore_start(&config, &CALLBACKS) };
        if rc == ESP_OK {
            if let Ok(mut in_use) = PARAMS_IN_USE.lock() {
                *in_use = Some(params);
            }
        } else {
            mesh_log!(ESP_LOG_WARN, b"mesh: could not start (%d)\n\0", rc);
        }
        rc
    }
    #[cfg(not(target_os = "espidf"))]
    {
        let _ = params;
        ESP_ERR_NOT_SUPPORTED
    }
}

// ---------------------------------------------------------------------------
// FFI exports — what apps use
// ---------------------------------------------------------------------------

/// The radio settings the mesh runs on, as apps see them (thistle/mesh.h).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct CMeshRadio {
    pub freq_hz: u32,
    pub bw_hz: u32,
    pub sf: u8,
    pub cr: u8,
    pub tx_power_dbm: i8,
}

impl From<MeshParams> for CMeshRadio {
    fn from(p: MeshParams) -> Self {
        Self {
            freq_hz: p.freq_hz,
            bw_hz: p.bw_hz,
            sf: p.sf,
            cr: p.cr,
            tx_power_dbm: p.tx_power_dbm,
        }
    }
}

/// The radio settings the mesh was started on: what another node has to be
/// set to before the two can hear each other.
#[no_mangle]
pub extern "C" fn rs_mesh_get_radio(out: *mut CMeshRadio) -> i32 {
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    match PARAMS_IN_USE.lock().ok().and_then(|p| *p) {
        Some(params) => {
            unsafe { *out = params.into() };
            ESP_OK
        }
        None => ESP_ERR_INVALID_STATE,
    }
}

/// Whether the mesh is on air.
#[no_mangle]
pub extern "C" fn rs_mesh_is_running() -> bool {
    MESH_STATE.lock().map(|s| s.running).unwrap_or(false)
}

/// Whether the board has a radio for the mesh to run on.
#[no_mangle]
pub extern "C" fn rs_mesh_available() -> bool {
    !crate::hal_registry::registry().radio.is_null()
}

/// Whether the user has the mesh switched on (`mesh.enabled`, on unless
/// switched off). It may still be starting.
#[no_mangle]
pub extern "C" fn rs_mesh_is_enabled() -> bool {
    enabled()
}

/// Switch the mesh on or off and remember it. Off takes the node off air and
/// puts the radio to sleep at once. On starts it; if it will not start, it
/// stays off, unsaved, and the error is returned. Otherwise the return says
/// whether the choice was saved.
#[no_mangle]
pub extern "C" fn rs_mesh_set_enabled(on: bool) -> i32 {
    if !on {
        ENABLED.store(OFF, Ordering::Relaxed);
        let rc = mesh_stop();
        if rc != ESP_OK {
            return rc;
        }
        mesh_log!(ESP_LOG_INFO, b"mesh: switched off\n\0");
        return crate::system_json::save("mesh", "enabled", serde_json::Value::Bool(false));
    }
    let was = ENABLED.swap(ON, Ordering::Relaxed);
    let rc = mesh_start();
    if rc != ESP_OK {
        ENABLED.store(if was == ON { ON } else { OFF }, Ordering::Relaxed);
        mesh_log!(ESP_LOG_WARN, b"mesh: would not start (0x%x)\n\0", rc);
        return rc;
    }
    mesh_log!(ESP_LOG_INFO, b"mesh: switched on\n\0");
    crate::system_json::save("mesh", "enabled", serde_json::Value::Bool(true))
}

/// Send a direct message to a contact. Its id, for following its delivery
/// with rs_mesh_delivery_pop(), goes to `out_id` if that is not null.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_send_direct(
    pub_key: *const u8,
    text: *const c_char,
    out_id: *mut u32,
) -> i32 {
    if pub_key.is_null() || text.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let len = std::ffi::CStr::from_ptr(text).to_bytes().len();
    if len == 0 {
        return ESP_ERR_INVALID_ARG;
    }
    if len > MESH_TEXT_MAX {
        return ESP_ERR_INVALID_SIZE;
    }
    let key = &*(pub_key as *const [u8; 32]);
    let name = {
        let state = match MESH_STATE.lock() {
            Ok(s) => s,
            Err(_) => return ESP_FAIL,
        };
        if !state.running {
            return ESP_ERR_INVALID_STATE;
        }
        state.contacts.iter().find(|c| &c.pub_key == key).map(|c| {
            String::from_utf8_lossy(&c.name[..(c.name_len as usize).min(32)]).into_owned()
        })
    };
    let Some(name) = name else {
        return ESP_ERR_NOT_FOUND;
    };
    let id = next_message_id();
    #[cfg(target_os = "espidf")]
    let rc = meshcore_send_direct(pub_key, text, id);
    #[cfg(not(target_os = "espidf"))]
    let rc = ESP_OK;
    if rc == ESP_OK {
        if !out_id.is_null() {
            *out_id = id;
        }
        record_sent(MSG_DIRECT, 0, key, &name, text);
    }
    rc
}

/// Send a message to a group channel; PUBLIC_CHANNEL is the one every node
/// shares. A channel message is never acknowledged, so its delivery reports
/// only whether it went out.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_send_channel(
    channel: u8,
    text: *const c_char,
    out_id: *mut u32,
) -> i32 {
    if text.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let len = std::ffi::CStr::from_ptr(text).to_bytes().len();
    if len == 0 {
        return ESP_ERR_INVALID_ARG;
    }
    if len > MESH_TEXT_MAX {
        return ESP_ERR_INVALID_SIZE;
    }
    if !rs_mesh_is_running() {
        return ESP_ERR_INVALID_STATE;
    }
    let id = next_message_id();
    #[cfg(target_os = "espidf")]
    let rc = meshcore_send_channel(channel, text, id);
    #[cfg(not(target_os = "espidf"))]
    let rc = ESP_OK;
    if rc == ESP_OK {
        if !out_id.is_null() {
            *out_id = id;
        }
        let to = if channel == PUBLIC_CHANNEL { "Public".to_string() } else { format!("Channel {channel}") };
        record_sent(MSG_CHANNEL, channel, &[0u8; 32], &to, text);
    }
    rc
}

/// Advertise this node, so peers can add it as a contact and message it.
#[no_mangle]
pub extern "C" fn rs_mesh_send_advert() -> i32 {
    if !rs_mesh_is_running() {
        return ESP_ERR_INVALID_STATE;
    }
    #[cfg(target_os = "espidf")]
    return unsafe { meshcore_send_advert() };
    #[cfg(not(target_os = "espidf"))]
    ESP_OK
}

/// Rename this node, advertise the new name, and keep it in system.json.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_set_name(name: *const c_char) -> i32 {
    if name.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let name = match std::ffi::CStr::from_ptr(name).to_str() {
        Ok(n) => truncate_utf8(n.trim(), MESH_NAME_MAX - 1).to_string(),
        Err(_) => return ESP_ERR_INVALID_ARG,
    };
    if name.is_empty() {
        return ESP_ERR_INVALID_ARG;
    }
    if !rs_mesh_is_running() {
        return ESP_ERR_INVALID_STATE;
    }
    #[cfg(target_os = "espidf")]
    {
        let c_name = to_c_array::<MESH_NAME_MAX>(&name);
        let rc = meshcore_set_name(c_name.as_ptr() as *const c_char);
        if rc != ESP_OK {
            return rc;
        }
    }
    if let Ok(mut state) = MESH_STATE.lock() {
        set_self_name(&mut state, name.as_bytes());
    }
    crate::system_json::save("mesh", "name", serde_json::Value::String(name))
}

/// Take the oldest received message, if any. Returns whether there was one.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_inbox_pop(out: *mut CMeshMessage) -> bool {
    if out.is_null() {
        return false;
    }
    let msg = match MESH_STATE.lock() {
        Ok(mut s) => s.inbox.pop_front(),
        Err(_) => None,
    };
    match msg {
        Some(m) => {
            *out = m;
            true
        }
        None => false,
    }
}

/// Messages received that the user has not read: those still in the inbox,
/// which nothing has shown, and those an app took and has not shown yet. The
/// status bar shows it.
#[no_mangle]
pub extern "C" fn rs_mesh_unread_count() -> u32 {
    MESH_STATE
        .lock()
        .map(|s| s.inbox.len() as u32 + s.held_unread)
        .unwrap_or(0)
}

/// How many of the messages it has taken from the inbox an app has still to
/// show the user. It reports again whenever that changes, and 0 when it
/// closes, since what it held goes with it.
#[no_mangle]
pub extern "C" fn rs_mesh_set_held_unread(count: u32) {
    if let Ok(mut s) = MESH_STATE.lock() {
        s.held_unread = count;
    }
}

/// Take the oldest delivery report: a message id from rs_mesh_send_direct()
/// or rs_mesh_send_channel(), and DELIVERY_SENT, _DELIVERED or _FAILED.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_delivery_pop(out_id: *mut u32, out_state: *mut u8) -> bool {
    if out_id.is_null() || out_state.is_null() {
        return false;
    }
    let report = match MESH_STATE.lock() {
        Ok(mut s) => s.deliveries.pop_front(),
        Err(_) => None,
    };
    match report {
        Some((id, state)) => {
            *out_id = id;
            *out_state = state;
            true
        }
        None => false,
    }
}

/// Return the number of contacts.
#[no_mangle]
pub extern "C" fn rs_mesh_get_contact_count() -> i32 {
    MESH_STATE.lock().map(|s| s.contacts.len() as i32).unwrap_or(-1)
}

/// Get a contact by index, most recently heard first.
#[no_mangle]
pub extern "C" fn rs_mesh_get_contact(index: i32, out: *mut CMeshContact) -> i32 {
    if out.is_null() || index < 0 {
        return ESP_ERR_INVALID_ARG;
    }
    let state = match MESH_STATE.lock() {
        Ok(s) => s,
        Err(_) => return ESP_FAIL,
    };
    match state.contacts.get(index as usize) {
        Some(c) => {
            unsafe { *out = *c };
            ESP_OK
        }
        None => ESP_ERR_NOT_FOUND,
    }
}

/// Find a contact by public key. Returns the index or -1 if not found.
#[no_mangle]
pub extern "C" fn rs_mesh_find_contact(pub_key: *const u8) -> i32 {
    if pub_key.is_null() {
        return -1;
    }
    rs_mesh_find_contact_prefix(pub_key, 32)
}

/// Find a contact by the first `len` bytes of its public key, as MeshCore
/// itself shortens keys. Returns the index or -1 if not found.
#[no_mangle]
pub extern "C" fn rs_mesh_find_contact_prefix(prefix: *const u8, len: usize) -> i32 {
    if prefix.is_null() || len == 0 || len > 32 {
        return -1;
    }
    let prefix = unsafe { std::slice::from_raw_parts(prefix, len) };
    MESH_STATE
        .lock()
        .ok()
        .and_then(|s| s.contacts.iter().position(|c| c.pub_key.starts_with(prefix)))
        .map(|i| i as i32)
        .unwrap_or(-1)
}

/// Get the local node's public key. Writes 32 bytes to `out`.
#[no_mangle]
pub extern "C" fn rs_mesh_get_self_key(out: *mut u8) -> i32 {
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let state = match MESH_STATE.lock() {
        Ok(s) => s,
        Err(_) => return ESP_FAIL,
    };
    if !state.running {
        return ESP_ERR_INVALID_STATE;
    }
    unsafe { std::ptr::copy_nonoverlapping(state.self_key.as_ptr(), out, 32) };
    ESP_OK
}

/// Get the local node's name: empty until the mesh is running. The pointer
/// stays valid; what it points at changes if the node is renamed.
#[no_mangle]
pub extern "C" fn rs_mesh_get_self_name() -> *const c_char {
    match SELF_NAME_C.lock() {
        Ok(name) => name.as_ptr() as *const c_char,
        Err(_) => b"\0".as_ptr() as *const c_char,
    }
}

/// Get mesh network statistics.
#[no_mangle]
pub extern "C" fn rs_mesh_get_stats(out: *mut CMeshStats) -> i32 {
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let contacts = match MESH_STATE.lock() {
        Ok(s) if s.running => s.contacts.len() as u32,
        Ok(_) => return ESP_ERR_INVALID_STATE,
        Err(_) => return ESP_FAIL,
    };
    #[allow(unused_mut)]
    let mut engine = EngineStats::default();
    #[cfg(target_os = "espidf")]
    {
        let rc = unsafe { meshcore_get_stats(&mut engine) };
        if rc != ESP_OK {
            return rc;
        }
    }
    unsafe {
        *out = CMeshStats {
            packets_sent: engine.packets_sent,
            packets_received: engine.packets_received,
            packets_forwarded: 0,
            messages_sent: engine.messages_sent,
            messages_received: engine.messages_received,
            contacts_discovered: contacts,
        };
    }
    ESP_OK
}

// ── The older app interface ──────────────────────────────────────────────
//
// Apps once ran the mesh themselves: init, call loop() every few
// milliseconds, deinit. The kernel runs it now, from boot, so these keep their
// names for the app SDK and the thistle-tk chat but no longer start or stop
// anything an app does not own.

/// Start the mesh if it is not already running. The name is the one the node
/// was given in system.json, not this one: the node is the device's, not an
/// app's.
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_init(name: *const c_char, _node_type: u8) -> i32 {
    if name.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    mesh_start()
}

/// Nothing to release: the mesh outlives the app that asked for it.
#[no_mangle]
pub extern "C" fn rs_mesh_deinit() -> i32 {
    ESP_OK
}

/// The engine runs its own loop; this reports whether it is running.
#[no_mangle]
pub extern "C" fn rs_mesh_loop() -> i32 {
    if rs_mesh_is_running() {
        ESP_OK
    } else {
        ESP_ERR_INVALID_STATE
    }
}

/// Send a direct message to a contact.
#[no_mangle]
pub extern "C" fn rs_mesh_send(dest_key: *const u8, text: *const c_char) -> i32 {
    unsafe { rs_mesh_send_direct(dest_key, text, std::ptr::null_mut()) }
}

/// Advertise this node. The position is not sent yet.
#[no_mangle]
pub extern "C" fn rs_mesh_send_advert_pos(_lat: f64, _lon: f64) -> i32 {
    rs_mesh_send_advert()
}

/// Return the number of messages waiting.
#[no_mangle]
pub extern "C" fn rs_mesh_get_inbox_count() -> i32 {
    MESH_STATE.lock().map(|s| s.inbox.len() as i32).unwrap_or(-1)
}

/// Read a waiting message by index (0 = oldest), leaving it waiting.
#[no_mangle]
pub extern "C" fn rs_mesh_get_inbox_message(index: i32, out: *mut CMeshMessage) -> i32 {
    if out.is_null() || index < 0 {
        return ESP_ERR_INVALID_ARG;
    }
    let state = match MESH_STATE.lock() {
        Ok(s) => s,
        Err(_) => return ESP_FAIL,
    };
    match state.inbox.get(index as usize) {
        Some(msg) => {
            unsafe { *out = *msg };
            ESP_OK
        }
        None => ESP_ERR_INVALID_ARG,
    }
}

/// Clear all waiting messages.
#[no_mangle]
pub extern "C" fn rs_mesh_clear_inbox() -> i32 {
    match MESH_STATE.lock() {
        Ok(mut s) => {
            s.inbox.clear();
            ESP_OK
        }
        Err(_) => ESP_FAIL,
    }
}

// ── The simulator ───────────────────────────────────────────────────────

/// The simulator has no radio, so this hands the inbox a message as if the
/// node had heard it: a direct one from `sender`, or one `sender` sent on the
/// Public channel. Each call is a new message, even with the same words.
#[cfg(not(target_os = "espidf"))]
#[no_mangle]
pub unsafe extern "C" fn rs_mesh_sim_hear(
    sender: *const c_char,
    text: *const c_char,
    direct: bool,
) -> bool {
    static HEARD: AtomicU32 = AtomicU32::new(1);
    if sender.is_null() || text.is_null() {
        return false;
    }
    let sender = std::ffi::CStr::from_ptr(sender).to_string_lossy();
    let text = std::ffi::CStr::from_ptr(text).to_string_lossy();
    let mut key = [0u8; 32];
    if direct {
        // A key of its own per name, so each sender is its own conversation.
        let hash = fnv1a(sender.as_bytes()).to_le_bytes();
        for chunk in key.chunks_mut(hash.len()) {
            chunk.copy_from_slice(&hash);
        }
    }
    let heard = EngineMessage {
        kind: if direct { MSG_DIRECT } else { MSG_CHANNEL },
        channel: PUBLIC_CHANNEL,
        sender_key: key,
        sender_name: to_c_array(&sender),
        timestamp: HEARD.fetch_add(1, Ordering::Relaxed),
        text: to_c_array(&text),
    };
    hear(message_from_engine(&heard))
}

// ── Syscall wrappers (thistle_mesh_* → rs_mesh_*) ───────────────────
//
// The thistle_* names loaded apps link against (app_sdk/include/thistle_app.h).

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_init(name: *const c_char, node_type: u8) -> i32 {
    rs_mesh_init(name, node_type)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_deinit() -> i32 {
    rs_mesh_deinit()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_loop() -> i32 {
    rs_mesh_loop()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_send(dest_key: *const u8, text: *const c_char) -> i32 {
    rs_mesh_send(dest_key, text)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_send_advert() -> i32 {
    rs_mesh_send_advert()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_send_advert_pos(lat: f64, lon: f64) -> i32 {
    rs_mesh_send_advert_pos(lat, lon)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_contact_count() -> i32 {
    rs_mesh_get_contact_count()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_contact(index: i32, out: *mut CMeshContact) -> i32 {
    rs_mesh_get_contact(index, out)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_find_contact(pub_key: *const u8) -> i32 {
    rs_mesh_find_contact(pub_key)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_inbox_count() -> i32 {
    rs_mesh_get_inbox_count()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_inbox_message(index: i32, out: *mut CMeshMessage) -> i32 {
    rs_mesh_get_inbox_message(index, out)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_clear_inbox() -> i32 {
    rs_mesh_clear_inbox()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_self_key(out: *mut u8) -> i32 {
    rs_mesh_get_self_key(out)
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_self_name() -> *const c_char {
    rs_mesh_get_self_name()
}

#[no_mangle]
pub unsafe extern "C" fn thistle_mesh_get_stats(out: *mut CMeshStats) -> i32 {
    rs_mesh_get_stats(out)
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    /// Reset the global state between tests; the statics persist across them.
    fn reset_state() {
        *MESH_STATE.lock().unwrap() = MeshState::new();
        *PARAMS_IN_USE.lock().unwrap() = None;
        *SELF_NAME_C.lock().unwrap() = [0u8; MESH_NAME_MAX];
        declare_radio_band(None);
        ENABLED.store(UNKNOWN, Ordering::Relaxed);
    }

    /// The state as it is once the engine has reported in.
    fn running_as(name: &str) {
        reset_state();
        let name = CString::new(name).unwrap();
        let key = [0xA5u8; 32];
        unsafe { on_ready(key.as_ptr(), name.as_ptr()) };
    }

    fn engine_contact(key: u8, name: &str, last_advert: u32) -> EngineContact {
        EngineContact {
            pub_key: [key; 32],
            name: to_c_array(name),
            node_type: 1,
            path_len: PATH_UNKNOWN,
            last_advert,
            lat_e6: 0,
            lon_e6: 0,
        }
    }

    fn engine_message(kind: u8, key: u8, name: &str, timestamp: u32, text: &str) -> EngineMessage {
        EngineMessage {
            kind,
            channel: 0,
            sender_key: if kind == MSG_DIRECT { [key; 32] } else { [0; 32] },
            sender_name: to_c_array(name),
            timestamp,
            text: to_c_array(text),
        }
    }

    fn text_of(msg: &CMeshMessage) -> &str {
        std::str::from_utf8(&msg.text[..msg.text_len as usize]).unwrap()
    }

    fn pop() -> Option<CMeshMessage> {
        let mut out = CMeshMessage::default();
        unsafe { rs_mesh_inbox_pop(&mut out) }.then_some(out)
    }

    // -- Radio settings ----------------------------------------------------

    #[test]
    fn an_868_mhz_board_joins_meshcores_eu_channel() {
        assert_eq!(mesh_params(Some(868_000_000), None), Some(EU_PRESET));
    }

    #[test]
    fn a_915_mhz_board_joins_meshcores_us_channel() {
        assert_eq!(mesh_params(Some(915_000_000), None), Some(US_PRESET));
    }

    #[test]
    fn a_board_with_no_radio_has_no_mesh() {
        assert_eq!(mesh_params(None, None), None);
    }

    #[test]
    fn a_band_meshcore_has_no_preset_for_needs_a_frequency_given() {
        assert_eq!(mesh_params(Some(433_000_000), None), None);
        let json = r#"{ "thistle_os": { "mesh": { "frequency_mhz": 433.375 } } }"#;
        let params = mesh_params(Some(433_000_000), Some(json)).unwrap();
        assert_eq!(params.freq_hz, 433_375_000);
    }

    #[test]
    fn system_json_can_move_the_node_within_its_band() {
        let json = r#"{ "thistle_os": { "mesh": {
            "frequency_mhz": 869.525, "bandwidth_khz": 250, "spreading_factor": 11,
            "coding_rate": 5, "tx_power_dbm": 14 } } }"#;
        assert_eq!(
            mesh_params(Some(868_000_000), Some(json)),
            Some(MeshParams {
                freq_hz: 869_525_000,
                bw_hz: 250_000,
                sf: 11,
                cr: 5,
                tx_power_dbm: 14,
            })
        );
    }

    #[test]
    fn a_frequency_outside_the_boards_band_is_refused() {
        let json = r#"{ "thistle_os": { "mesh": { "frequency_mhz": 910.525 } } }"#;
        assert_eq!(mesh_params(Some(868_000_000), Some(json)), Some(EU_PRESET));
    }

    #[test]
    fn settings_out_of_range_are_ignored() {
        let json = r#"{ "thistle_os": { "mesh": {
            "spreading_factor": 13, "coding_rate": 9, "tx_power_dbm": 30,
            "bandwidth_khz": -5 } } }"#;
        assert_eq!(mesh_params(Some(868_000_000), Some(json)), Some(EU_PRESET));
    }

    #[test]
    fn the_mesh_is_on_unless_switched_off() {
        assert!(mesh_enabled(None));
        assert!(mesh_enabled(Some(r#"{ "thistle_os": {} }"#)));
        assert!(!mesh_enabled(Some(
            r#"{ "thistle_os": { "mesh": { "enabled": false } } }"#
        )));
    }

    #[test]
    fn a_configured_name_is_trimmed_and_cut_to_fit_an_advert() {
        assert_eq!(configured_name(None), None);
        let blank = r#"{ "thistle_os": { "mesh": { "name": "   " } } }"#;
        assert_eq!(configured_name(Some(blank)), None);
        let named = r#"{ "thistle_os": { "mesh": { "name": " Mo's T-Deck " } } }"#;
        assert_eq!(configured_name(Some(named)).as_deref(), Some("Mo's T-Deck"));
        let long = format!(
            r#"{{ "thistle_os": {{ "mesh": {{ "name": "{}" }} }} }}"#,
            "é".repeat(20)
        );
        let cut = configured_name(Some(&long)).unwrap();
        assert!(cut.len() <= MESH_NAME_MAX - 1);
        assert!(cut.chars().all(|c| c == 'é'));
    }

    // -- Starting --------------------------------------------------------------

    #[test]
    fn the_radio_settings_are_reported_once_the_mesh_has_them() {
        reset_state();
        let mut radio = CMeshRadio::default();
        assert_eq!(rs_mesh_get_radio(&mut radio), ESP_ERR_INVALID_STATE);
        *PARAMS_IN_USE.lock().unwrap() = Some(EU_PRESET);
        assert_eq!(rs_mesh_get_radio(&mut radio), ESP_OK);
        assert_eq!(
            radio,
            CMeshRadio { freq_hz: 869_618_000, bw_hz: 62_500, sf: 8, cr: 5, tx_power_dbm: 22 }
        );
        assert_eq!(rs_mesh_get_radio(std::ptr::null_mut()), ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn nothing_is_running_until_the_engine_reports_in() {
        reset_state();
        assert!(!rs_mesh_is_running());
        let mut key = [0u8; 32];
        assert_eq!(rs_mesh_get_self_key(key.as_mut_ptr()), ESP_ERR_INVALID_STATE);
        let name = unsafe { std::ffi::CStr::from_ptr(rs_mesh_get_self_name()) };
        assert_eq!(name.to_bytes(), b"");
    }

    #[test]
    fn a_board_with_no_radio_does_not_start_the_mesh() {
        reset_state();
        ENABLED.store(ON, Ordering::Relaxed);
        assert_eq!(mesh_start(), ESP_ERR_NOT_FOUND);
        assert!(!rs_mesh_is_running());
    }

    #[test]
    fn a_mesh_switched_off_is_not_started() {
        reset_state();
        ENABLED.store(OFF, Ordering::Relaxed);
        assert_eq!(mesh_start(), ESP_ERR_NOT_SUPPORTED);
        assert!(!rs_mesh_is_running());
    }

    #[test]
    fn switching_on_a_mesh_that_will_not_start_leaves_it_off() {
        reset_state();
        ENABLED.store(OFF, Ordering::Relaxed);
        // No radio band declared, so there is nothing to tune to.
        assert_eq!(rs_mesh_set_enabled(true), ESP_ERR_NOT_FOUND);
        assert!(!rs_mesh_is_enabled());
        assert!(!rs_mesh_is_running());
    }

    #[test]
    fn stopping_takes_the_node_off_air() {
        running_as("ThistleOS-A5A5");
        assert_eq!(mesh_stop(), ESP_OK);
        assert!(!rs_mesh_is_running());
    }

    #[test]
    fn the_engine_reporting_in_names_the_node() {
        running_as("ThistleOS-A5A5");
        assert!(rs_mesh_is_running());
        let mut key = [0u8; 32];
        assert_eq!(rs_mesh_get_self_key(key.as_mut_ptr()), ESP_OK);
        assert_eq!(key, [0xA5; 32]);
        let name = unsafe { std::ffi::CStr::from_ptr(rs_mesh_get_self_name()) };
        assert_eq!(name.to_bytes(), b"ThistleOS-A5A5");
    }

    // -- Contacts --------------------------------------------------------------

    #[test]
    fn contacts_are_listed_most_recently_heard_first() {
        running_as("me");
        unsafe {
            on_contact(&engine_contact(1, "old", 100));
            on_contact(&engine_contact(2, "new", 300));
            on_contact(&engine_contact(3, "mid", 200));
        }
        assert_eq!(rs_mesh_get_contact_count(), 3);
        let names: Vec<String> = (0..3)
            .map(|i| {
                let mut c = CMeshContact::default();
                assert_eq!(rs_mesh_get_contact(i, &mut c), ESP_OK);
                String::from_utf8(c.name[..c.name_len as usize].to_vec()).unwrap()
            })
            .collect();
        assert_eq!(names, ["new", "mid", "old"]);
    }

    #[test]
    fn a_contact_heard_again_is_updated_not_duplicated() {
        running_as("me");
        unsafe {
            on_contact(&engine_contact(1, "Heltec", 100));
            on_contact(&engine_contact(2, "other", 200));
            on_contact(&engine_contact(1, "Heltec V3", 300));
        }
        assert_eq!(rs_mesh_get_contact_count(), 2);
        let mut c = CMeshContact::default();
        rs_mesh_get_contact(0, &mut c);
        assert_eq!(&c.name[..c.name_len as usize], b"Heltec V3");
        assert_eq!(c.last_seen, 300);
    }

    #[test]
    fn a_contact_the_engine_dropped_is_gone() {
        running_as("me");
        unsafe {
            on_contact(&engine_contact(1, "a", 100));
            on_contact_removed([1u8; 32].as_ptr());
        }
        assert_eq!(rs_mesh_get_contact_count(), 0);
    }

    #[test]
    fn a_contact_is_found_by_a_prefix_of_its_key() {
        running_as("me");
        unsafe {
            on_contact(&engine_contact(0x11, "a", 100));
            on_contact(&engine_contact(0x22, "b", 200));
        }
        assert_eq!(rs_mesh_find_contact_prefix([0x11u8; 6].as_ptr(), 6), 1);
        assert_eq!(rs_mesh_find_contact([0x22u8; 32].as_ptr()), 0);
        assert_eq!(rs_mesh_find_contact_prefix([0x33u8; 6].as_ptr(), 6), -1);
        assert_eq!(rs_mesh_find_contact_prefix(std::ptr::null(), 6), -1);
    }

    #[test]
    fn a_contacts_position_comes_through_in_degrees() {
        let mut c = engine_contact(1, "a", 100);
        c.lat_e6 = 59_913_868;
        c.lon_e6 = 10_752_245;
        let out = contact_from_engine(&c);
        assert!(out.has_position);
        assert!((out.lat - 59.913868).abs() < 1e-9);
        assert!((out.lon - 10.752245).abs() < 1e-9);
        assert!(!contact_from_engine(&engine_contact(1, "a", 100)).has_position);
    }

    // -- Receiving -------------------------------------------------------------

    #[test]
    fn messages_wait_in_order_for_an_app() {
        running_as("me");
        unsafe {
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            on_message(&engine_message(MSG_CHANNEL, 0, "Bob", 11, "hi all"));
        }
        let first = pop().unwrap();
        assert_eq!(first.kind, MSG_DIRECT);
        assert_eq!(first.sender_key, [1; 32]);
        assert_eq!(text_of(&first), "hello");
        let second = pop().unwrap();
        assert_eq!(second.kind, MSG_CHANNEL);
        assert_eq!(&second.sender_name[..second.sender_name_len as usize], b"Bob");
        assert_eq!(text_of(&second), "hi all");
        assert!(pop().is_none());
    }

    #[test]
    fn a_message_sent_again_for_want_of_an_ack_is_shown_once() {
        running_as("me");
        unsafe {
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            // Same words, sent later, are a new message.
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 12, "hello"));
        }
        assert_eq!(rs_mesh_get_inbox_count(), 2);
    }

    #[test]
    fn a_full_inbox_drops_its_oldest_message() {
        running_as("me");
        for i in 0..(INBOX_SIZE as u32 + 3) {
            unsafe { on_message(&engine_message(MSG_DIRECT, 1, "a", i, "x")) };
        }
        assert_eq!(rs_mesh_get_inbox_count(), INBOX_SIZE as i32);
        assert_eq!(pop().unwrap().timestamp, 3);
    }

    #[test]
    fn messages_nothing_has_shown_are_unread() {
        running_as("me");
        assert_eq!(rs_mesh_unread_count(), 0);
        unsafe {
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            on_message(&engine_message(MSG_CHANNEL, 0, "Bob", 11, "hi all"));
        }
        assert_eq!(rs_mesh_unread_count(), 2);
    }

    #[test]
    fn messages_an_app_took_stay_unread_until_it_shows_them() {
        running_as("me");
        unsafe {
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            on_message(&engine_message(MSG_CHANNEL, 0, "Bob", 11, "hi all"));
        }
        pop().unwrap();
        rs_mesh_set_held_unread(1);
        assert_eq!(rs_mesh_unread_count(), 2);
        rs_mesh_set_held_unread(0);
        assert_eq!(rs_mesh_unread_count(), 1);
    }

    #[test]
    fn a_message_heard_twice_is_unread_once() {
        running_as("me");
        unsafe {
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
            on_message(&engine_message(MSG_DIRECT, 1, "Heltec", 10, "hello"));
        }
        assert_eq!(rs_mesh_unread_count(), 1);
    }

    #[test]
    fn a_message_a_full_inbox_dropped_is_not_counted() {
        running_as("me");
        for i in 0..(INBOX_SIZE as u32 + 3) {
            unsafe { on_message(&engine_message(MSG_DIRECT, 1, "a", i, "x")) };
        }
        assert_eq!(rs_mesh_unread_count(), INBOX_SIZE as u32);
    }

    #[test]
    fn the_simulator_hears_direct_and_channel_messages() {
        running_as("me");
        let bob = CString::new("Bob").unwrap();
        let alice = CString::new("Alice").unwrap();
        let hi = CString::new("hi").unwrap();
        unsafe {
            assert!(rs_mesh_sim_hear(bob.as_ptr(), hi.as_ptr(), true));
            assert!(rs_mesh_sim_hear(bob.as_ptr(), hi.as_ptr(), true));
            assert!(rs_mesh_sim_hear(alice.as_ptr(), hi.as_ptr(), false));
            assert!(!rs_mesh_sim_hear(std::ptr::null(), hi.as_ptr(), false));
        }
        let first = pop().unwrap();
        let second = pop().unwrap();
        let third = pop().unwrap();
        assert_eq!(first.kind, MSG_DIRECT);
        assert_ne!(first.sender_key, [0; 32]);
        assert_eq!(first.sender_key, second.sender_key);
        assert_eq!(text_of(&second), "hi");
        assert_eq!(third.kind, MSG_CHANNEL);
        assert_eq!(third.channel, PUBLIC_CHANNEL);
        assert_eq!(third.sender_key, [0; 32]);
        assert_eq!(&third.sender_name[..third.sender_name_len as usize], b"Alice");
        assert!(pop().is_none());
    }

    #[test]
    fn the_older_inbox_interface_reads_without_taking() {
        running_as("me");
        unsafe { on_message(&engine_message(MSG_DIRECT, 1, "a", 42, "Hi")) };
        let mut out = CMeshMessage::default();
        assert_eq!(rs_mesh_get_inbox_message(0, &mut out), ESP_OK);
        assert_eq!(out.timestamp, 42);
        assert_eq!(rs_mesh_get_inbox_count(), 1);
        assert_eq!(rs_mesh_get_inbox_message(1, &mut out), ESP_ERR_INVALID_ARG);
        assert_eq!(rs_mesh_get_inbox_message(0, std::ptr::null_mut()), ESP_ERR_INVALID_ARG);
        assert_eq!(rs_mesh_clear_inbox(), ESP_OK);
        assert_eq!(rs_mesh_get_inbox_count(), 0);
    }

    #[test]
    fn text_longer_than_an_app_buffer_is_cut_not_overrun() {
        let long = "y".repeat(MESH_TEXT_MAX);
        let msg = message_from_engine(&engine_message(MSG_DIRECT, 1, "a", 1, &long));
        assert_eq!(msg.text_len as usize, MESH_TEXT_MAX);
        assert_eq!(msg.text[MESH_TEXT_MAX], 0);
    }

    // -- Sending ---------------------------------------------------------------

    #[test]
    fn nothing_can_be_sent_before_the_mesh_is_running() {
        reset_state();
        let text = CString::new("hi").unwrap();
        let key = [1u8; 32];
        unsafe {
            assert_eq!(
                rs_mesh_send_direct(key.as_ptr(), text.as_ptr(), std::ptr::null_mut()),
                ESP_ERR_INVALID_STATE
            );
            assert_eq!(
                rs_mesh_send_channel(PUBLIC_CHANNEL, text.as_ptr(), std::ptr::null_mut()),
                ESP_ERR_INVALID_STATE
            );
        }
        assert_eq!(rs_mesh_send_advert(), ESP_ERR_INVALID_STATE);
    }

    #[test]
    fn a_direct_message_goes_only_to_a_known_contact() {
        running_as("me");
        unsafe { on_contact(&engine_contact(1, "Heltec", 100)) };
        let text = CString::new("hello").unwrap();
        let mut id = 0u32;
        unsafe {
            assert_eq!(rs_mesh_send_direct([1u8; 32].as_ptr(), text.as_ptr(), &mut id), ESP_OK);
            assert_ne!(id, 0);
            assert_eq!(
                rs_mesh_send_direct([2u8; 32].as_ptr(), text.as_ptr(), &mut id),
                ESP_ERR_NOT_FOUND
            );
        }
    }

    #[test]
    fn each_message_sent_gets_its_own_id() {
        running_as("me");
        let text = CString::new("hello").unwrap();
        let (mut a, mut b) = (0u32, 0u32);
        unsafe {
            assert_eq!(rs_mesh_send_channel(PUBLIC_CHANNEL, text.as_ptr(), &mut a), ESP_OK);
            assert_eq!(rs_mesh_send_channel(PUBLIC_CHANNEL, text.as_ptr(), &mut b), ESP_OK);
        }
        assert_ne!(a, 0);
        assert_ne!(a, b);
    }

    #[test]
    fn text_meshcore_cannot_carry_is_refused() {
        running_as("me");
        let long = CString::new("z".repeat(MESH_TEXT_MAX + 1)).unwrap();
        let empty = CString::new("").unwrap();
        unsafe {
            assert_eq!(
                rs_mesh_send_channel(PUBLIC_CHANNEL, long.as_ptr(), std::ptr::null_mut()),
                ESP_ERR_INVALID_SIZE
            );
            assert_eq!(
                rs_mesh_send_channel(PUBLIC_CHANNEL, empty.as_ptr(), std::ptr::null_mut()),
                ESP_ERR_INVALID_ARG
            );
            assert_eq!(
                rs_mesh_send_channel(PUBLIC_CHANNEL, std::ptr::null(), std::ptr::null_mut()),
                ESP_ERR_INVALID_ARG
            );
        }
    }

    #[test]
    fn delivery_reports_come_back_in_order() {
        running_as("me");
        unsafe {
            on_delivery(7, DELIVERY_SENT);
            on_delivery(8, DELIVERY_DELIVERED);
        }
        let (mut id, mut state) = (0u32, 0u8);
        unsafe {
            assert!(rs_mesh_delivery_pop(&mut id, &mut state));
            assert_eq!((id, state), (7, DELIVERY_SENT));
            assert!(rs_mesh_delivery_pop(&mut id, &mut state));
            assert_eq!((id, state), (8, DELIVERY_DELIVERED));
            assert!(!rs_mesh_delivery_pop(&mut id, &mut state));
        }
    }

    // -- The older app interface -----------------------------------------------

    #[test]
    fn the_older_interface_reports_the_running_mesh() {
        reset_state();
        assert_eq!(rs_mesh_loop(), ESP_ERR_INVALID_STATE);
        running_as("me");
        assert_eq!(rs_mesh_loop(), ESP_OK);
        assert_eq!(rs_mesh_deinit(), ESP_OK);
        assert!(rs_mesh_is_running(), "an app letting go must not stop the mesh");
        let name = CString::new("ignored").unwrap();
        assert_eq!(unsafe { rs_mesh_init(name.as_ptr(), 0) }, ESP_OK);
        assert_eq!(unsafe { rs_mesh_init(std::ptr::null(), 0) }, ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn stats_count_the_contacts_heard() {
        running_as("me");
        unsafe { on_contact(&engine_contact(1, "a", 1)) };
        let mut stats = CMeshStats::default();
        assert_eq!(rs_mesh_get_stats(&mut stats), ESP_OK);
        assert_eq!(stats.contacts_discovered, 1);
        assert_eq!(rs_mesh_get_stats(std::ptr::null_mut()), ESP_ERR_INVALID_ARG);
    }

    // -- Struct layout ---------------------------------------------------------

    #[test]
    fn the_engine_structs_match_meshcore_shim_h() {
        // meshcore_contact_t: 32 + 32 + 1 + 1 + (2 pad) + 4 + 4 + 4
        assert_eq!(std::mem::size_of::<EngineContact>(), 80);
        // meshcore_message_t: 1 + 1 + 32 + 32 + (2 pad) + 4 + 161 + (3 pad)
        assert_eq!(std::mem::size_of::<EngineMessage>(), 236);
        // meshcore_config_t: 4 + 4 + 1 + 1 + 1 + 32 + 48 + (1 pad)
        assert_eq!(std::mem::size_of::<EngineConfig>(), 92);
        assert_eq!(std::mem::size_of::<EngineStats>(), 20);
    }

    #[test]
    fn the_app_structs_match_thistle_mesh_h() {
        // mesh_contact_t and mesh_message_t _Static_assert the same sizes.
        assert_eq!(std::mem::size_of::<CMeshContact>(), 96);
        assert_eq!(std::mem::size_of::<CMeshMessage>(), 276);
        assert_eq!(std::mem::size_of::<CMeshRadio>(), 12);
    }

    #[test]
    fn test_mesh_stats_size() {
        assert_eq!(std::mem::size_of::<CMeshStats>(), 24, "6 x u32 = 24 bytes");
    }
}
