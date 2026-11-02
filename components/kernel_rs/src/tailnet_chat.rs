// SPDX-License-Identifier: BSD-3-Clause
//! Tailnet chat: this device as a chat peer on the user's tailnet
//! (ADR-0015, docs/tailnet-chat/protocol.md).
//!
//! - While the tailnet is connected, a listener on the tailnet address,
//!   port `thistle_os.tailnet_chat.port` (4810), takes WebSockets from chat
//!   peers. Only the tailnet's addresses are admitted (and loopback in the
//!   simulator). At most CONNECTIONS_MAX connections, taken and dialled,
//!   are open at once.
//! - Nothing here has a thread: the sockets are non-blocking and a kernel
//!   worker job polls them every POLL_MS (`tick`). Only a dial's TCP
//!   connect, which blocks, runs on a one-shot task of its own
//!   (`chat_dial`), and hands the socket back. Connection buffers are
//!   allocated large enough to land in PSRAM.
//! - The device proves its MeshCore identity (`/spiffs/config/mesh_identity`)
//!   in the handshake, signing through the kernel's crypto.
//! - Who a proven key is, what accepting, renaming and removing do, and when
//!   the outbox is tried again is decided in tailnet_chat_policy.rs; the
//!   wire is tailnet_chat_wire.rs.
//! - Contacts, their pinned keys and names, what waits for their ack and
//!   how many of their messages are unread are kept in this module's own
//!   file, `/spiffs/tailnet_chat.json`.
//! - Messages, their state changes (delivered, read) and reactions go into
//!   the message history as its third kind (msg_log.rs,
//!   `messages/tailnet/`); a conversation is read back from it and folded.
//!   A stranger's messages are acked and held as a contact request.
//! - A message received buzzes and pops up (msg_alert.rs, its third kind)
//!   and counts as unread until read, unless its conversation is on screen.
//!
//! The app (apps/tailchat) only reads and asks, through thistle/tailnet_chat.h.

use std::collections::VecDeque;
use std::ffi::{c_char, CStr, CString};
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::os::raw::c_void;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use crate::msg_alert::{self, Transport};
use crate::msg_log::{self, ChatEntry, ChatLine, ChatReact, Direction};
use crate::tailnet_chat_policy::{self as policy, Book, Dial, DialCandidate, Outbox, Retry, Seen, Trust};
use crate::tailnet_chat_wire::{self as wire, ErrorFrame, Key, Role, Upgrade, Upgraded, WsEvent, WsReader};

const SECTION: &str = "tailnet_chat";

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NO_MEM: i32 = 0x101;

/// How often the sockets are polled.
const POLL_MS: u64 = 100;
/// The most chat connections open at once, dials in flight included (the
/// spec's budget of sockets).
pub(crate) const CONNECTIONS_MAX: usize = 4;
const HANDSHAKE_MS: u64 = 10_000;
const PING_MS: u64 = 20_000;
const IDLE_MS: u64 = 120_000;
/// How long a dial's TCP connect may take: a first packet to a tailnet peer
/// starts the WireGuard handshake and is dropped; the second SYN finds it.
const CONNECT_S: u64 = 5;
/// The dial task's stack, internal RAM, gone once it has connected.
const DIAL_STACK: usize = 6 * 1024;
/// Unsent bytes past which a connection is given up as stuck.
const TX_MAX: usize = 64 * 1024;
/// Messages kept per conversation for the app; the rest are on the card.
pub(crate) const SHOWN_MAX: usize = 20;
/// The ids of messages and reactions remembered per contact, so that one
/// sent again is never shown twice, after a restart too: read back from
/// the last lines of its file.
const SEEN_MAX: usize = 200;
/// Contact requests held at once, and messages held for each.
pub(crate) const REQUESTS_MAX: usize = 16;
pub(crate) const HELD_MAX: usize = 20;
/// A buffer this big is allocated in PSRAM on the device
/// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 4096).
const PSRAM_BUF: usize = 8 * 1024;

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
    #[cfg(not(test))]
    {
        let c = CString::new(text.replace('\0', "")).unwrap_or_default();
        unsafe { esp_log_write(level, b"tailnet_chat\0".as_ptr(), b"%s\n\0".as_ptr(), c.as_ptr()) };
    }
    #[cfg(test)]
    let _ = (level, text);
}

fn info(text: &str) {
    log(ESP_LOG_INFO, text);
}

fn warn(text: &str) {
    log(ESP_LOG_WARN, text);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Settings {
    pub port: u16,
    pub check_minutes: u32,
    pub read_receipts: bool,
}

pub(crate) fn settings_in(json: Option<&str>) -> Settings {
    let get = |key| json.and_then(|j| crate::system_json::get(j, SECTION, key));
    Settings {
        port: get("port")
            .and_then(|v| v.as_u64())
            .filter(|p| (1..=65535).contains(p))
            .map_or(wire::PORT, |p| p as u16),
        check_minutes: get("check_minutes").and_then(|v| v.as_u64()).map_or(15, |m| m.min(24 * 60) as u32),
        read_receipts: get("read_receipts").and_then(|v| v.as_bool()).unwrap_or(true),
    }
}

// ---------------------------------------------------------------------------
// Where things are
// ---------------------------------------------------------------------------

#[cfg(target_os = "espidf")]
fn spiffs() -> PathBuf {
    PathBuf::from("/spiffs")
}

#[cfg(not(target_os = "espidf"))]
fn spiffs() -> PathBuf {
    PathBuf::from(
        std::env::var("THISTLE_SIM_SPIFFS")
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "/tmp/thistle_spiffs".to_string()),
    )
}

fn book_path() -> PathBuf {
    spiffs().join("tailnet_chat.json")
}

/// The simulator alone admits loopback and listens on it.
const SIMULATOR: bool = cfg!(not(target_os = "espidf"));

// ---------------------------------------------------------------------------
// The identity: MeshCore's (mesh_identity.rs, made at boot)
// ---------------------------------------------------------------------------

use crate::mesh_identity::Identity;

fn load_identity() -> Option<Identity> {
    crate::mesh_identity::identity()
}

fn own_key(id: &Identity) -> Key {
    id.public
}

fn sign(id: &Identity, msg: &[u8]) -> [u8; 64] {
    id.sign(msg)
}

fn verify(key: &Key, msg: &[u8], sig: &[u8; 64]) -> bool {
    unsafe { crate::crypto::thistle_crypto_ed25519_verify(key.as_ptr(), msg.as_ptr(), msg.len(), sig.as_ptr()) == 0 }
}

fn random<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    unsafe { crate::crypto::thistle_crypto_random(b.as_mut_ptr(), N) };
    b
}

fn new_id() -> String {
    wire::hex(&random::<16>())
}

fn now_ms() -> u64 {
    static START: OnceLock<Instant> = OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_millis() as u64
}

fn wall_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

// ---------------------------------------------------------------------------
// Messages and conversations
// ---------------------------------------------------------------------------

/// What the app is told about why chat is or is not available.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum Shown {
    Unavailable = 0,
    TailnetOff = 1,
    NotSignedIn = 2,
    Waiting = 3,
    NoIdentity = 4,
    NoPort = 5,
    Listening = 6,
}

/// A reaction as a message keeps it: the emoji ("" once removed) and when
/// it was written, so that one delivered late replaces nothing newer.
pub(crate) type Reaction = Option<(String, u64)>;

/// One message as the app shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Message {
    pub id: String,
    pub mine: bool,
    pub ts: u64,
    pub text: String,
    /// "received" or "read"; for one sent, "waiting", "delivered" or "read".
    pub state: String,
    pub reply_to: Option<String>,
    pub react_me: Reaction,
    pub react_them: Reaction,
}

impl Message {
    fn from_line(l: ChatLine) -> Message {
        Message {
            id: l.id,
            mine: l.dir == Direction::Out,
            ts: l.ts,
            text: l.text,
            state: l.state,
            reply_to: l.reply_to,
            react_me: None,
            react_them: None,
        }
    }

    /// A reaction shown: its emoji, unless removed.
    pub fn shown(r: &Reaction) -> Option<&str> {
        r.as_ref().map(|(e, _)| e.as_str()).filter(|e| !e.is_empty())
    }
}

/// How far along a state is; a state change never goes back.
fn rank(state: &str) -> u8 {
    match state {
        "waiting" | "received" => 0,
        "delivered" => 1,
        "read" => 2,
        _ => 0,
    }
}

fn advance(m: &mut Message, state: &str) -> bool {
    if rank(state) > rank(&m.state) {
        m.state = state.to_string();
        true
    } else {
        false
    }
}

/// Put a reaction on `m`, mine or theirs, unless a newer one is there.
fn react_on(m: &mut Message, mine: bool, emoji: &str, ts: u64) -> bool {
    let slot = if mine { &mut m.react_me } else { &mut m.react_them };
    if !policy::reaction_replaces(slot.as_ref().map(|(_, t)| *t), ts) {
        return false;
    }
    *slot = Some((emoji.to_string(), ts));
    true
}

/// A conversation: its latest messages and the ids it has seen.
pub(crate) struct Conv {
    key: Key,
    messages: Vec<Message>,
    seen: VecDeque<String>,
}

impl Conv {
    fn saw(&mut self, id: &str) {
        self.seen.push_back(id.to_string());
        while self.seen.len() > SEEN_MAX {
            self.seen.pop_front();
        }
    }

    fn has_seen(&self, id: &str) -> bool {
        self.seen.iter().any(|s| s == id)
    }

    fn find(&mut self, id: &str) -> Option<&mut Message> {
        self.messages.iter_mut().find(|m| m.id == id)
    }
}

/// A conversation as its file's last lines tell it: messages in the order
/// written, each with its latest state and reactions, the latest `max` of
/// them kept; and every id seen.
fn fold(key: Key, entries: Vec<ChatEntry>, max: usize) -> Conv {
    let mut conv = Conv { key, messages: Vec::new(), seen: VecDeque::new() };
    for e in entries {
        match e {
            ChatEntry::Message(l) => {
                conv.saw(&l.id);
                if conv.find(&l.id).is_none() {
                    insert_by_ts(&mut conv.messages, Message::from_line(l), usize::MAX);
                }
            }
            ChatEntry::State { id, state, .. } => {
                if let Some(m) = conv.find(&id) {
                    advance(m, &state);
                }
            }
            ChatEntry::React(r) => {
                conv.saw(&r.id);
                if let Some(m) = conv.find(&r.to) {
                    react_on(m, r.dir == Direction::Out, &r.emoji, r.ts);
                }
            }
        }
    }
    let n = conv.messages.len();
    if n > max {
        conv.messages.drain(..n - max);
    }
    conv
}

/// Insert in `ts` order, keeping the latest `max`.
fn insert_by_ts(list: &mut Vec<Message>, m: Message, max: usize) {
    let at = list.iter().rposition(|x| x.ts <= m.ts).map_or(0, |p| p + 1);
    list.insert(at, m);
    if list.len() > max {
        list.drain(..list.len() - max);
    }
}

/// A contact request: a stranger and what it sent, held.
pub(crate) struct Request {
    pub key: Key,
    pub name: String,
    pub address: Option<IpAddr>,
    pub held: Vec<Message>,
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

#[derive(Debug)]
enum Phase {
    /// Dialled: our upgrade request sent, reading the answer.
    Upgrading,
    /// Taken: reading the HTTP upgrade.
    Upgrade,
    /// Our hello sent; waiting for theirs.
    Hello,
    /// Their hello read; waiting for their auth.
    Auth { key: Key, nonce: [u8; 32], name: String, caps: Vec<String> },
    /// Authenticated as `key`.
    Open { key: Key, name: String, caps: Vec<String> },
}

struct Conn {
    sock: TcpStream,
    peer: SocketAddr,
    rx: Vec<u8>,
    ws: WsReader,
    tx: Vec<u8>,
    phase: Phase,
    nonce: [u8; 32],
    /// Dialled by this device, for the contact of this key (its placeholder
    /// for one not yet pinned); None for one taken.
    dialled_for: Option<Key>,
    /// The Sec-WebSocket-Key of a dialled connection's upgrade.
    ws_key: String,
    opened_ms: u64,
    last_ms: u64,
    pinged_ms: u64,
    /// Closed (or to close once tx is out): no more frames are read.
    closing: bool,
    /// Outbox ids sent on this connection.
    sent: Vec<String>,
}

impl Conn {
    fn key(&self) -> Option<&Key> {
        match &self.phase {
            Phase::Open { key, .. } => Some(key),
            _ => None,
        }
    }

    /// Whether both hellos announced `cap`.
    fn may(&self, cap: &str) -> bool {
        matches!(&self.phase, Phase::Open { caps, .. } if caps.iter().any(|c| c == cap)) && wire::CAPS.contains(&cap)
    }

    /// A dialler masks what it sends (RFC 6455); a listener does not.
    fn mask(&self) -> Option<[u8; 4]> {
        self.dialled_for.map(|_| random::<4>())
    }

    fn send_text(&mut self, text: &str) {
        let mask = self.mask();
        self.tx.extend_from_slice(&wire::ws_frame(wire::OP_TEXT, text.as_bytes(), mask));
    }

    fn send_op(&mut self, op: u8, data: &[u8]) {
        let mask = self.mask();
        self.tx.extend_from_slice(&wire::ws_frame(op, data, mask));
    }

    fn send_error(&mut self, e: &ErrorFrame) {
        self.send_text(&e.to_json());
    }

    fn close(&mut self, code: u16, why: &str) {
        if !self.closing {
            if !matches!(self.phase, Phase::Upgrade | Phase::Upgrading) {
                let mask = self.mask();
                self.tx.extend_from_slice(&wire::ws_close(code, mask));
            }
            self.closing = true;
            let who = match &self.phase {
                Phase::Open { name, key, .. } => format!("{name} ({})", wire::fingerprint(key)),
                _ => self.peer.ip().to_string(),
            };
            info(&format!("closed {who}: {why} ({code})"));
        }
    }

    fn new(sock: TcpStream, peer: SocketAddr, now: u64, dialled_for: Option<Key>) -> Conn {
        let dialled = dialled_for.is_some();
        Conn {
            sock,
            peer,
            rx: Vec::new(),
            ws: WsReader::with_capacity(!dialled, PSRAM_BUF),
            tx: Vec::with_capacity(PSRAM_BUF),
            phase: if dialled { Phase::Upgrading } else { Phase::Upgrade },
            nonce: random::<32>(),
            dialled_for,
            ws_key: if dialled { wire::ws_key(&random::<16>()) } else { String::new() },
            opened_ms: now,
            last_ms: now,
            pinged_ms: now,
            closing: false,
            sent: Vec::new(),
        }
    }
}

/// Dials whose TCP connect has finished, handed back by their task: the
/// contact's key, and the socket or why not.
static DIALLED: Mutex<Vec<(Key, Result<(TcpStream, SocketAddr), String>)>> = Mutex::new(Vec::new());

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct State {
    settings: Settings,
    identity: Option<Identity>,
    name: String,
    listener: Option<TcpListener>,
    listening_on: Option<SocketAddr>,
    conns: Vec<Conn>,
    book: Book,
    outbox: Outbox,
    /// When each contact's outbox is dialled next.
    retries: Vec<(Key, Retry)>,
    /// Contacts whose dial's connect is on its task now.
    dialling: Vec<Key>,
    requests: Vec<Request>,
    /// The latest messages of each contact, read from the card the first
    /// time they are wanted.
    convs: Vec<Conv>,
    /// When each contact last said it was typing (now_ms).
    typing: Vec<(Key, u64)>,
    /// The conversation the app has on screen.
    viewing: Option<Key>,
    shown: Shown,
    why: String,
    next_look_ms: u64,
    /// Read into, once allocated; in PSRAM on the device.
    buf: Vec<u8>,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
/// Bumped at every change the app shows.
static GENERATION: AtomicU32 = AtomicU32::new(1);

fn lock() -> MutexGuard<'static, Option<State>> {
    STATE.lock().unwrap_or_else(PoisonError::into_inner)
}

fn changed() {
    GENERATION.fetch_add(1, Ordering::Relaxed);
}

fn save(s: &State) {
    let path = book_path();
    let tmp = path.with_extension("tmp");
    let text = policy::to_file(&s.book, &s.outbox);
    let ok = std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, &path));
    if let Err(e) = ok {
        warn(&format!("could not save the contacts to {}: {e}", path.display()));
    }
}

fn fp_id(key: &Key) -> String {
    wire::fingerprint_id(key)
}

impl State {
    fn new(settings: Settings) -> State {
        let (book, outbox) = std::fs::read_to_string(book_path())
            .map(|t| policy::from_file(&t))
            .unwrap_or_default();
        let mut s = State {
            settings,
            identity: None,
            name: String::new(),
            listener: None,
            listening_on: None,
            conns: Vec::new(),
            book,
            outbox,
            retries: Vec::new(),
            dialling: Vec::new(),
            requests: Vec::new(),
            convs: Vec::new(),
            typing: Vec::new(),
            viewing: None,
            shown: Shown::Waiting,
            why: String::new(),
            next_look_ms: 0,
            buf: Vec::new(),
        };
        s.read_requests();
        s
    }

    /// The contact requests on the card, from before this boot.
    fn read_requests(&mut self) {
        let root = msg_log::root();
        for fp in msg_log::chat_requests(root) {
            let lines = msg_log::chat_read(root, true, &fp, HELD_MAX);
            let Some(first) = lines.first() else { continue };
            let Some(key) = wire::unhex::<32>(&first.key) else { continue };
            if self.book.find(&key).is_some() || self.requests.len() >= REQUESTS_MAX {
                continue;
            }
            let name = first.name.clone();
            let held = lines.into_iter().map(Message::from_line).collect();
            self.requests.push(Request { key, name, address: None, held });
        }
    }

    fn conv(&mut self, key: &Key) -> &mut Conv {
        let i = match self.convs.iter().position(|c| &c.key == key) {
            Some(i) => i,
            None => {
                let entries = msg_log::chat_entries(msg_log::root(), false, &fp_id(key), SEEN_MAX);
                self.convs.push(fold(*key, entries, SHOWN_MAX));
                self.convs.len() - 1
            }
        };
        &mut self.convs[i]
    }

    fn box_of(&mut self, key: &Key) -> &mut Vec<(String, String)> {
        let i = match self.outbox.iter().position(|(k, _)| k == key) {
            Some(i) => i,
            None => {
                self.outbox.push((*key, Vec::new()));
                self.outbox.len() - 1
            }
        };
        &mut self.outbox[i].1
    }

    fn waiting(&self, key: &Key) -> bool {
        self.outbox.iter().any(|(k, f)| k == key && !f.is_empty())
    }

    fn retry(&mut self, key: &Key) -> &mut Retry {
        let i = match self.retries.iter().position(|(k, _)| k == key) {
            Some(i) => i,
            None => {
                self.retries.push((*key, Retry::default()));
                self.retries.len() - 1
            }
        };
        &mut self.retries[i].1
    }

    fn queue(&mut self, key: &Key, id: String, frame: String) {
        self.box_of(key).push((id, frame));
        save(self);
        self.flush(key);
    }

    /// Send what waits for `key` on its open connection, if it has one.
    fn flush(&mut self, key: &Key) {
        let frames: Vec<(String, String)> = self
            .outbox
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, f)| f.clone())
            .unwrap_or_default();
        if let Some(c) = self.open_conn(key) {
            for (id, frame) in frames {
                if !c.sent.contains(&id) {
                    c.send_text(&frame);
                    c.sent.push(id);
                }
            }
        }
    }

    fn open_conn(&mut self, key: &Key) -> Option<&mut Conn> {
        self.conns.iter_mut().find(|c| !c.closing && c.key() == Some(key))
    }

    fn connected(&self, key: &Key) -> bool {
        self.conns.iter().any(|c| !c.closing && c.key() == Some(key))
    }

    /// A connection open or opening to `key`, or a dial on its way.
    fn reaching(&self, key: &Key) -> bool {
        self.dialling.contains(key)
            || self.conns.iter().any(|c| !c.closing && (c.key() == Some(key) || c.dialled_for.as_ref() == Some(key)))
    }

    fn contact_name(&self, key: &Key) -> String {
        self.book.find(key).map_or_else(|| wire::fingerprint(key), |i| self.book.contacts[i].name.clone())
    }

    fn unread(&self) -> u32 {
        self.book.contacts.iter().map(|c| c.unread).sum()
    }

    /// Mark what `key` sent read: the ids given, or everything. Sends the
    /// read receipt if the user allows it and the peer is connected; it is
    /// never queued.
    fn mark_read(&mut self, key: &Key, ids: Option<&[String]>) {
        let fp = fp_id(key);
        let hex = wire::hex(key);
        let conv = self.conv(key);
        let mut read = Vec::new();
        for m in conv.messages.iter_mut().filter(|m| !m.mine) {
            if ids.is_none_or(|ids| ids.contains(&m.id)) && advance(m, "read") {
                read.push(m.id.clone());
            }
        }
        for id in &read {
            msg_log::chat_note(false, &fp, &ChatEntry::State { dir: Direction::In, key: hex.clone(),
                                                                id: id.clone(), state: "read".into() });
        }
        if let Some(i) = self.book.find(key) {
            if self.book.contacts[i].unread > 0 {
                self.book.contacts[i].unread = 0;
                save(self);
            }
        }
        if !read.is_empty() {
            if self.settings.read_receipts {
                if let Some(c) = self.open_conn(key) {
                    if c.may("receipts") {
                        c.send_text(&wire::read(&read));
                    }
                }
            }
            changed();
        }
    }

    /// A contact added by address proved `key` when dialled: pin it, and
    /// what was kept under its placeholder carries on under the key.
    fn pin(&mut self, i: usize, key: &Key) {
        let Some(placeholder) = self.book.pin(i, key) else { return };
        let name = self.book.contacts[i].name.clone();
        // Already a contact under that key: the two are one.
        if let Some(j) = self.book.contacts.iter().enumerate().position(|(j, c)| j != i && &c.key == key) {
            self.book.contacts[j].dial = self.book.contacts[i].dial.clone();
            self.book.contacts.remove(i);
        }
        for (k, _) in self.outbox.iter_mut().filter(|(k, _)| k == &placeholder) {
            *k = *key;
        }
        for (k, _) in self.retries.iter_mut().filter(|(k, _)| k == &placeholder) {
            *k = *key;
        }
        self.convs.retain(|c| c.key != placeholder && &c.key != key);
        msg_log::chat_rekey(&fp_id(&placeholder), &fp_id(key));
        if self.viewing == Some(placeholder) {
            self.viewing = Some(*key);
            msg_alert_viewing(Some(key));
        }
        info(&format!("pinned {name} as {}", wire::fingerprint(key)));
        save(self);
        changed();
    }
}

fn msg_alert_viewing(key: Option<&Key>) {
    match key {
        Some(k) => {
            let fp = CString::new(fp_id(k)).unwrap_or_default();
            unsafe { msg_alert::rs_msg_alert_set_viewing(Transport::Tailnet as i32, fp.as_ptr()) };
        }
        None => unsafe { msg_alert::rs_msg_alert_set_viewing(-1, std::ptr::null()) },
    }
}

// ---------------------------------------------------------------------------
// Starting, stopping, the look every POLL_MS
// ---------------------------------------------------------------------------

/// Read the settings and poll from now on. Called once by kernel_init(),
/// after the tailnet manager has started.
pub(crate) fn start() {
    let json = std::fs::read_to_string(crate::system_json::path()).ok();
    let settings = settings_in(json.as_deref());
    {
        let mut guard = lock();
        if guard.is_some() {
            return;
        }
        *guard = Some(State::new(settings));
    }
    if !crate::tailnet_manager::tailnet_available() {
        return;
    }
    crate::kworker::every(tick, std::ptr::null_mut(), Duration::from_millis(POLL_MS));
    #[cfg(not(target_os = "espidf"))]
    sim_control::start();
}

/// Whether the tailnet is up, and if not, why, for the user.
fn tailnet_shown() -> (Shown, String, String, String) {
    let mut st: crate::tailnet_manager::CTailnetStatus = unsafe { std::mem::zeroed() };
    if unsafe { crate::tailnet_manager::tailnet_get_status(&mut st) } != ESP_OK {
        return (Shown::Waiting, "Tailscale is starting".into(), String::new(), String::new());
    }
    let text = |b: &[u8]| CStr::from_bytes_until_nul(b).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default();
    let (ip, host) = (text(&st.ip), text(&st.hostname));
    let (shown, why) = match st.state {
        0 => (Shown::TailnetOff, "Tailscale is off. Switch it on in the Tailscale app."),
        1 => (Shown::Waiting, "Tailscale is waiting for a network."),
        2 => (Shown::Waiting, "Tailscale is waiting for the clock to be set."),
        3 => (Shown::Waiting, "Tailscale is connecting."),
        4 => (Shown::NotSignedIn, "Not signed in to Tailscale. Sign in from the Tailscale app."),
        5 => (Shown::NotSignedIn, "Tailscale is waiting for an admin to approve this device."),
        6 => (Shown::Listening, ""),
        7 => (Shown::TailnetOff, "Tailscale stopped. Open the Tailscale app to see why."),
        _ => (Shown::Unavailable, "This board has no Tailscale client."),
    };
    (shown, why.to_string(), ip, host)
}

unsafe extern "C" fn tick(_arg: *mut c_void) {
    let now = now_ms();
    let mut guard = lock();
    let Some(s) = guard.as_mut() else { return };

    if now >= s.next_look_ms {
        s.next_look_ms = now + 1000;
        look(s);
    }
    if s.listener.is_some() {
        poll(s, now);
        dial_due(s, now);
    }
}

/// Once a second: start or stop with the tailnet.
fn look(s: &mut State) {
    let (mut shown, mut why, ip, host) = tailnet_shown();
    if shown == Shown::Listening && s.identity.is_none() {
        s.identity = load_identity();
        if let Some(id) = &s.identity {
            info(&format!("identity {}", wire::fingerprint(&own_key(id))));
        }
    }
    if shown == Shown::Listening && s.identity.is_none() {
        shown = Shown::NoIdentity;
        why = "This device has no MeshCore identity yet.".into();
    }
    if shown == Shown::Listening && s.listener.is_none() {
        s.name = host;
        match listen(s, &ip) {
            // The tailnet is back: whatever waits is tried now.
            Ok(()) => s.retries.iter_mut().for_each(|(_, r)| r.seen_online()),
            Err(e) => {
                shown = Shown::NoPort;
                why = format!("Could not listen on port {}: {e}", s.settings.port);
            }
        }
    }
    if shown != Shown::Listening && s.listener.is_some() {
        stop_listening(s, "the tailnet is down");
    }
    if shown != s.shown || why != s.why {
        if shown != Shown::Listening && !why.is_empty() && shown != s.shown {
            info(&format!("not listening: {why}"));
        }
        s.shown = shown;
        s.why = why;
        changed();
    }
    let now = now_ms();
    let before = s.typing.len();
    s.typing.retain(|(_, t)| policy::is_typing(Some(*t), now));
    if s.typing.len() != before {
        changed();
    }
}

fn listen(s: &mut State, tailnet_ip: &str) -> std::io::Result<()> {
    let ip: IpAddr = if SIMULATOR {
        IpAddr::V4(Ipv4Addr::LOCALHOST)
    } else {
        tailnet_ip.parse().unwrap_or(IpAddr::V4(Ipv4Addr::UNSPECIFIED))
    };
    let port = sim_port().unwrap_or(s.settings.port);
    let listener = TcpListener::bind(SocketAddr::new(ip, port))?;
    listener.set_nonblocking(true)?;
    let addr = listener.local_addr()?;
    let fp = s.identity.as_ref().map(|i| wire::fingerprint(&own_key(i))).unwrap_or_default();
    info(&format!("listening on {addr} as {fp}"));
    s.listener = Some(listener);
    s.listening_on = Some(addr);
    if s.buf.capacity() < PSRAM_BUF {
        s.buf = vec![0u8; PSRAM_BUF];
    }
    Ok(())
}

#[cfg(not(target_os = "espidf"))]
fn sim_port() -> Option<u16> {
    std::env::var("THISTLE_SIM_CHAT_PORT").ok().and_then(|p| p.parse().ok())
}

#[cfg(target_os = "espidf")]
fn sim_port() -> Option<u16> {
    None
}

fn stop_listening(s: &mut State, why: &str) {
    for c in &mut s.conns {
        c.close(wire::CLOSE_GOING_AWAY, why);
        let _ = c.sock.write(&c.tx);
    }
    s.conns.clear();
    s.listener = None;
    s.listening_on = None;
    info(&format!("stopped listening: {why}"));
    changed();
}

// ---------------------------------------------------------------------------
// Dialling
// ---------------------------------------------------------------------------

/// Where to connect for `at`: an address, else a tailnet peer's name from
/// the tailnet's map (never DNS: a dead resolver would hold the dial).
fn resolve(at: &Dial) -> Result<SocketAddr, String> {
    if let Ok(ip) = at.host.parse::<IpAddr>() {
        return Ok(SocketAddr::new(ip, at.port));
    }
    match crate::tailnet_manager::resolve_peer(&at.host) {
        Some(ip) => Ok(SocketAddr::new(IpAddr::V4(ip), at.port)),
        None => Err(format!("{} is not on the tailnet", at.host)),
    }
}

/// Dial each contact with something waiting whose retry is due, as the
/// connections left allow (tailnet_chat_policy::to_dial).
fn dial_due(s: &mut State, now: u64) {
    take_dialled(s, now);
    let open = s.conns.len() + s.dialling.len();
    let free = CONNECTIONS_MAX.saturating_sub(open);
    if free == 0 || s.outbox.iter().all(|(_, f)| f.is_empty()) {
        return;
    }
    let candidates: Vec<DialCandidate> = (0..s.book.contacts.len())
        .map(|i| {
            let key = s.book.contacts[i].key;
            DialCandidate {
                waiting: s.waiting(&key),
                connected: s.reaching(&key),
                dialable: s.book.dial_of(i).is_some(),
                retry: s.retries.iter().find(|(k, _)| k == &key).map_or_else(Retry::default, |(_, r)| *r),
            }
        })
        .collect();
    for i in policy::to_dial(&candidates, now, free) {
        let (Some(at), key) = (s.book.dial_of(i), s.book.contacts[i].key) else { continue };
        dial(s, key, at, now);
    }
}

fn dial(s: &mut State, key: Key, at: Dial, now: u64) {
    let name = s.contact_name(&key);
    let addr = match resolve(&at) {
        Ok(a) => a,
        Err(e) => {
            s.retry(&key).failed(now);
            info(&format!("could not dial {name}: {e}"));
            return;
        }
    };
    if !policy::admitted(addr.ip(), SIMULATOR) {
        s.retry(&key).failed(now);
        info(&format!("not dialling {name} at {addr}: not on the tailnet"));
        return;
    }
    s.dialling.push(key);
    info(&format!("dialling {name} at {addr}"));
    let job = move || {
        let r = TcpStream::connect_timeout(&addr, Duration::from_secs(CONNECT_S))
            .map(|sock| (sock, addr))
            .map_err(|e| e.to_string());
        DIALLED.lock().unwrap_or_else(PoisonError::into_inner).push((key, r));
    };
    if !crate::kernel_task::spawn(c"chat_dial", DIAL_STACK, crate::kernel_task::PRIORITY_DEFAULT, job) {
        s.dialling.retain(|k| k != &key);
        s.retry(&key).failed(now);
        warn(&format!("could not dial {name}: no task"));
    }
}

/// The dials whose connect has finished: each a connection to upgrade, or
/// a failure to try again after.
fn take_dialled(s: &mut State, now: u64) {
    let done: Vec<_> = std::mem::take(&mut *DIALLED.lock().unwrap_or_else(PoisonError::into_inner));
    for (key, result) in done {
        s.dialling.retain(|k| k != &key);
        let name = s.contact_name(&key);
        match result {
            Err(e) => {
                s.retry(&key).failed(now);
                info(&format!("could not reach {name}: {e}"));
                changed();
            }
            Ok((sock, addr)) => {
                if sock.set_nonblocking(true).is_err() {
                    s.retry(&key).failed(now);
                    continue;
                }
                let _ = sock.set_nodelay(true);
                let mut c = Conn::new(sock, addr, now, Some(key));
                let host = addr.ip().to_string();
                c.tx.extend_from_slice(wire::upgrade_request(&host, addr.port(), &c.ws_key).as_bytes());
                s.conns.push(c);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The sockets
// ---------------------------------------------------------------------------

fn poll(s: &mut State, now: u64) {
    accept_new(s, now);
    for i in 0..s.conns.len() {
        read_conn(s, i, now);
    }
    for c in &mut s.conns {
        if c.closing {
            continue;
        }
        if !matches!(c.phase, Phase::Open { .. }) && now - c.opened_ms > HANDSHAKE_MS {
            if !matches!(c.phase, Phase::Upgrade | Phase::Upgrading) {
                c.send_error(&ErrorFrame::new("bad_request").detail("handshake timeout"));
            }
            c.close(wire::CLOSE_HANDSHAKE, "no handshake within 10 s");
        } else if matches!(c.phase, Phase::Open { .. }) {
            if now - c.last_ms > IDLE_MS {
                c.close(wire::CLOSE_NORMAL, "idle");
            } else if now - c.pinged_ms > PING_MS {
                c.pinged_ms = now;
                c.send_op(wire::OP_PING, b"");
            }
        }
    }
    let mut gone: Vec<(Option<Key>, Option<Key>)> = Vec::new();
    s.conns.retain_mut(|c| {
        let keep = (|| {
            if !c.tx.is_empty() {
                match c.sock.write(&c.tx) {
                    Ok(n) => {
                        c.tx.drain(..n);
                    }
                    Err(e) if e.kind() == ErrorKind::WouldBlock => {}
                    Err(_) => return false,
                }
            }
            if c.tx.len() > TX_MAX {
                warn(&format!("gave up on {}: it reads nothing", c.peer.ip()));
                return false;
            }
            !(c.closing && c.tx.is_empty())
        })();
        if !keep {
            gone.push((c.key().copied(), c.dialled_for));
        }
        keep
    });
    for (open, dialled_for) in &gone {
        // A dial that never opened, or a connection that closed with
        // something still waiting: the backoff decides when to try again.
        let key = open.or(*dialled_for);
        if let Some(k) = key {
            if (open.is_none() || s.waiting(&k)) && !s.connected(&k) {
                s.retry(&k).failed(now);
            }
        }
        if let Some(k) = open {
            s.typing.retain(|(t, _)| t != k);
        }
    }
    if !gone.is_empty() {
        changed();
    }
}

fn accept_new(s: &mut State, now: u64) {
    loop {
        let Some(listener) = &s.listener else { return };
        let (sock, peer) = match listener.accept() {
            Ok(x) => x,
            Err(e) if e.kind() == ErrorKind::WouldBlock => return,
            Err(e) => {
                warn(&format!("accept: {e}"));
                return;
            }
        };
        if !policy::admitted(peer.ip(), SIMULATOR) {
            info(&format!("refused {}: not on the tailnet", peer.ip()));
            continue;
        }
        if s.conns.len() + s.dialling.len() >= CONNECTIONS_MAX {
            info(&format!("refused {}: {CONNECTIONS_MAX} connections open already", peer.ip()));
            continue;
        }
        if sock.set_nonblocking(true).is_err() {
            continue;
        }
        let _ = sock.set_nodelay(true);
        s.conns.push(Conn::new(sock, peer, now, None));
    }
}

fn read_conn(s: &mut State, i: usize, now: u64) {
    loop {
        let c = &mut s.conns[i];
        if c.closing {
            return;
        }
        let n = match c.sock.read(&mut s.buf) {
            Ok(0) => {
                c.closing = true;
                if let Phase::Open { name, .. } = &c.phase {
                    info(&format!("{name} disconnected"));
                }
                return;
            }
            Ok(n) => n,
            Err(e) if e.kind() == ErrorKind::WouldBlock => return,
            Err(_) => {
                c.closing = true;
                return;
            }
        };
        c.last_ms = now;
        match c.phase {
            Phase::Upgrade => {
                c.rx.extend_from_slice(&s.buf[..n]);
                match wire::upgrade(&c.rx) {
                    Upgrade::Incomplete => continue,
                    Upgrade::Refuse(response) => {
                        c.tx.extend_from_slice(response.as_bytes());
                        c.closing = true;
                        info(&format!("refused {}: not an upgrade to {}", c.peer.ip(), wire::PATH));
                        return;
                    }
                    Upgrade::Accept { response, used } => {
                        c.tx.extend_from_slice(response.as_bytes());
                        let rest = c.rx.split_off(used);
                        c.rx = Vec::new();
                        c.ws.push(&rest);
                        send_hello(s, i);
                    }
                }
            }
            Phase::Upgrading => {
                c.rx.extend_from_slice(&s.buf[..n]);
                match wire::upgraded(&c.rx, &c.ws_key) {
                    Upgraded::Incomplete => continue,
                    Upgraded::Refused => {
                        c.closing = true;
                        info(&format!("{} refused the upgrade to {}", c.peer, wire::PATH));
                        return;
                    }
                    Upgraded::Accepted { used } => {
                        let rest = c.rx.split_off(used);
                        c.rx = Vec::new();
                        c.ws.push(&rest);
                        send_hello(s, i);
                    }
                }
            }
            _ => c.ws.push(&s.buf[..n]),
        }
        events(s, i);
    }
}

fn send_hello(s: &mut State, i: usize) {
    let key = s.identity.as_ref().map(own_key).unwrap_or_default();
    let hello = wire::hello(&key, &s.name, &s.conns[i].nonce);
    let c = &mut s.conns[i];
    c.phase = Phase::Hello;
    c.send_text(&hello);
}

/// Handle every whole frame that has arrived on connection `i`.
fn events(s: &mut State, i: usize) {
    loop {
        if s.conns[i].closing {
            return;
        }
        let event = match s.conns[i].ws.next() {
            Ok(Some(e)) => e,
            Ok(None) => return,
            Err(e) => {
                s.conns[i].close(e.close_code(), "a frame the WebSocket does not allow");
                return;
            }
        };
        match event {
            WsEvent::Ping(data) => s.conns[i].send_op(wire::OP_PONG, &data),
            WsEvent::Pong => {}
            WsEvent::Close(code) => {
                let c = &mut s.conns[i];
                let mask = c.mask();
                c.tx.extend_from_slice(&wire::ws_close(code.min(4999), mask));
                c.closing = true;
                if let Phase::Open { name, .. } = &c.phase {
                    info(&format!("{name} closed the connection ({code})"));
                }
            }
            WsEvent::Text(text) => match s.conns[i].phase {
                Phase::Open { .. } => on_frame(s, i, &text),
                _ => on_handshake(s, i, &text),
            },
            WsEvent::Binary(data) => match s.conns[i].phase {
                Phase::Open { .. } => on_binary(s, i, &data),
                _ => {
                    let c = &mut s.conns[i];
                    c.send_error(&ErrorFrame::new("bad_request").detail("binary in the handshake"));
                    c.close(wire::CLOSE_HANDSHAKE, "binary in the handshake");
                }
            },
        }
    }
}

fn refuse(c: &mut Conn, e: ErrorFrame, why: &str) {
    info(&format!("refused {}: {why}", c.peer.ip()));
    c.send_error(&e);
    c.close(wire::CLOSE_HANDSHAKE, why);
}

fn on_handshake(s: &mut State, i: usize, text: &[u8]) {
    let frame = match wire::check(text) {
        Ok(f) => f,
        Err(e) => return refuse(&mut s.conns[i], e, "a malformed frame in the handshake"),
    };
    let Some(id) = s.identity.as_ref() else { return };
    let own = own_key(id);
    let kind = wire::frame_type(&frame).to_string();
    let c = &mut s.conns[i];
    let dialler = c.dialled_for.is_some();
    match (&c.phase, kind.as_str()) {
        (Phase::Hello, "hello") => {
            if frame.get("version").and_then(|v| v.as_u64()) != Some(wire::VERSION) {
                return refuse(c, ErrorFrame::new("version"), "another protocol version");
            }
            let key = wire::unhex::<32>(wire::str_of(&frame, "key")).unwrap_or_default();
            if key == own {
                return refuse(c, ErrorFrame::new("bad_request").what("hello").detail("my own key"), "my own key");
            }
            let nonce = wire::unhex::<32>(wire::str_of(&frame, "nonce")).unwrap_or_default();
            let name = wire::str_of(&frame, "name").to_string();
            let caps = frame
                .get("caps")
                .and_then(|v| v.as_array())
                .map(|a| a.iter().filter_map(|c| c.as_str().map(str::to_string)).collect())
                .unwrap_or_default();
            if dialler {
                // The dialler proves itself first.
                let t = wire::transcript(Role::Dialler, &c.nonce, &nonce, &own, &key);
                let sig = sign(s.identity.as_ref().unwrap(), &t);
                let c = &mut s.conns[i];
                c.send_text(&wire::auth(&sig));
                c.phase = Phase::Auth { key, nonce, name, caps };
            } else {
                c.phase = Phase::Auth { key, nonce, name, caps };
            }
        }
        (Phase::Auth { key, nonce, name, caps }, "auth") => {
            let (key, their_nonce, hello_name, caps) = (*key, *nonce, name.clone(), caps.clone());
            let sig = wire::unhex::<64>(wire::str_of(&frame, "sig")).unwrap_or([0; 64]);
            if dialler {
                let theirs = wire::transcript(Role::Listener, &c.nonce, &their_nonce, &own, &key);
                if !verify(&key, &theirs, &sig) {
                    return refuse(c, ErrorFrame::new("bad_signature"), "a signature that does not verify");
                }
            } else {
                let theirs = wire::transcript(Role::Dialler, &their_nonce, &c.nonce, &key, &own);
                if !verify(&key, &theirs, &sig) {
                    return refuse(c, ErrorFrame::new("bad_signature"), "a signature that does not verify");
                }
                let mine = wire::transcript(Role::Listener, &their_nonce, &c.nonce, &key, &own);
                let our_sig = sign(s.identity.as_ref().unwrap(), &mine);
                s.conns[i].send_text(&wire::auth(&our_sig));
            }
            authenticated(s, i, key, hello_name, caps);
        }
        _ => refuse(c, ErrorFrame::new("bad_request").what(&kind).detail("out of place in the handshake"),
                    &format!("{kind} out of place in the handshake")),
    }
}

/// Who a connection is, as the trust rules see it.
fn seen_as(c: &Conn, hello_name: &str) -> (String, Option<IpAddr>) {
    let ip = c.peer.ip();
    let address = policy::on_tailnet(ip).then_some(ip);
    let name = match ip {
        IpAddr::V4(v4) if address.is_some() => crate::tailnet_manager::peer_name(v4),
        _ => None,
    };
    (name.unwrap_or_else(|| hello_name.to_string()), address)
}

fn authenticated(s: &mut State, i: usize, key: Key, hello_name: String, caps: Vec<String>) {
    let (name, address) = seen_as(&s.conns[i], &hello_name);
    let fp = wire::fingerprint(&key);
    // Dialled for a contact: the first handshake pins a contact added by
    // address; another key where a pinned one was dialled is an impostor.
    let mut trust = None;
    if let Some(dk) = s.conns[i].dialled_for {
        if let Some(ci) = s.book.find(&dk) {
            if s.book.contacts[ci].pending {
                s.pin(ci, &key);
                s.conns[i].dialled_for = Some(key);
            } else if dk != key {
                trust = Some(Trust::KeyChanged(ci));
            }
        }
    }
    let seen = Seen { key: &key, name: &name, address };
    match trust.unwrap_or_else(|| s.book.classify(&seen)) {
        Trust::KeyChanged(ci) => {
            let contact = s.book.contacts[ci].name.clone();
            warn(&format!("refused {name} ({fp}): not the key pinned for {contact}; flagged"));
            s.book.flag(ci, &key);
            save(s);
            changed();
            let c = &mut s.conns[i];
            c.send_error(&ErrorFrame::new("key_changed").detail(&contact));
            c.close(wire::CLOSE_KEY_CHANGED, "key changed");
            return;
        }
        Trust::Contact(ci) => {
            if s.book.contacts[ci].seen_name != name || (address.is_some() && s.book.contacts[ci].address != address) {
                s.book.seen(ci, &seen);
                save(s);
            }
            info(&format!("connected: {name} {fp}, contact {}", s.book.contacts[ci].name));
        }
        Trust::Stranger => info(&format!("connected: {name} {fp}, a stranger")),
    }
    // One connection per pair: of two, the one dialled by the lower key.
    let own = s.identity.as_ref().map(own_key).unwrap_or_default();
    let dialler_of = |c: &Conn| if c.dialled_for.is_some() { own } else { *c.key().unwrap_or(&key) };
    let new_dialler = if s.conns[i].dialled_for.is_some() { own } else { key };
    if let Some(j) = (0..s.conns.len()).find(|&j| j != i && !s.conns[j].closing && s.conns[j].key() == Some(&key)) {
        if policy::keep_new_connection(&dialler_of(&s.conns[j]), &new_dialler) {
            s.conns[j].close(wire::CLOSE_DUPLICATE, "a second connection with the same peer");
        } else {
            s.conns[i].phase = Phase::Open { key, name, caps };
            s.conns[i].close(wire::CLOSE_DUPLICATE, "a second connection with the same peer");
            return;
        }
    }
    s.conns[i].phase = Phase::Open { key, name, caps };
    s.retry(&key).connected();
    s.flush(&key);
    changed();
}

fn on_binary(s: &mut State, i: usize, data: &[u8]) {
    let c = &mut s.conns[i];
    let e = match wire::unpack_binary(data) {
        None => ErrorFrame::new("bad_request").what("binary"),
        Some(b) if b.kind == wire::KIND_VOICE || b.kind == wire::KIND_CALL_AUDIO => {
            ErrorFrame::new("bad_request").what("binary").detail("no such stream")
        }
        Some(b) => ErrorFrame::new("unsupported").what(&format!("binary kind {}", b.kind)),
    };
    c.send_error(&e);
}

fn on_frame(s: &mut State, i: usize, text: &[u8]) {
    let Phase::Open { key, name, .. } = &s.conns[i].phase else { return };
    let (key, name) = (*key, name.clone());
    let frame = match wire::check(text) {
        Ok(f) => f,
        Err(e) => {
            info(&format!("answered {name}: error {} {}", e.code, e.what.as_deref().unwrap_or("")));
            return s.conns[i].send_error(&e);
        }
    };
    let id = wire::str_of(&frame, "id").to_string();
    let contact = s.book.find(&key).is_some();
    match wire::frame_type(&frame) {
        "msg" => {
            let m = Message {
                id: id.clone(),
                mine: false,
                ts: frame.get("ts").and_then(|v| v.as_u64()).unwrap_or(0),
                text: wire::str_of(&frame, "text").to_string(),
                state: "received".into(),
                reply_to: frame.get("reply_to").and_then(|v| v.as_str()).map(str::to_string),
                react_me: None,
                react_them: None,
            };
            if received(s, &key, &name, m) {
                s.conns[i].send_text(&wire::ack(&id));
                if contact && s.viewing == Some(key) {
                    s.mark_read(&key, Some(&[id]));
                }
            }
        }
        "react" => {
            if contact {
                let r = ChatReact {
                    dir: Direction::In,
                    key: wire::hex(&key),
                    name: name.clone(),
                    id: id.clone(),
                    ts: frame.get("ts").and_then(|v| v.as_u64()).unwrap_or(0),
                    to: wire::str_of(&frame, "to").to_string(),
                    emoji: wire::str_of(&frame, "emoji").to_string(),
                };
                reacted(s, &key, r);
            }
            // A stranger's reaction is acked and dropped: there is nothing
            // of ours it can be on.
            s.conns[i].send_text(&wire::ack(&id));
        }
        "ack" => {
            let before = s.outbox.iter().map(|(_, f)| f.len()).sum::<usize>();
            if let Some((_, frames)) = s.outbox.iter_mut().find(|(k, _)| k == &key) {
                frames.retain(|(fid, _)| fid != &id);
            }
            s.outbox.retain(|(_, f)| !f.is_empty());
            if s.outbox.iter().map(|(_, f)| f.len()).sum::<usize>() != before {
                info(&format!("delivered {id} to {name}"));
                save(s);
                if contact {
                    state_of_mine(s, &key, &[id], "delivered");
                }
                changed();
            }
        }
        "read" => {
            if contact {
                let ids: Vec<String> = frame
                    .get("ids")
                    .and_then(|v| v.as_array())
                    .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect())
                    .unwrap_or_default();
                state_of_mine(s, &key, &ids, "read");
            }
        }
        "typing" => {
            // A stranger's typing is ignored (protocol section 7).
            if contact {
                let on = frame.get("on").and_then(|v| v.as_bool()).unwrap_or(false);
                s.typing.retain(|(k, _)| k != &key);
                if on {
                    s.typing.push((key, now_ms()));
                }
                changed();
            }
        }
        "contact" => {
            let accepted = wire::str_of(&frame, "state") == "accepted";
            if let Some(ci) = s.book.find(&key) {
                s.book.contacts[ci].accepted_by_them = Some(accepted);
                if !accepted {
                    // Declined: nothing more is sent to it until the user starts over.
                    s.outbox.retain(|(k, _)| k != &key);
                }
                save(s);
                changed();
            }
            info(&format!("{name} {} this device's contact request", if accepted { "accepted" } else { "declined" }));
            s.conns[i].send_text(&wire::ack(&id));
        }
        // Not announced yet (tickets 08, 09): a peer that read our hello
        // sends none of these.
        "voice" | "call_answer" | "call_end" => {}
        "call_offer" => {
            let call = wire::str_of(&frame, "call");
            s.conns[i].send_text(&format!(r#"{{"type":"call_answer","call":"{call}","decline":"unsupported"}}"#));
        }
        "error" => info(&format!("{name} answered: error {}", wire::str_of(&frame, "code"))),
        other => {
            let e = ErrorFrame::new("bad_request").what(other).detail("after the handshake");
            s.conns[i].send_error(&e);
        }
    }
}

/// Messages of ours to `key` became `state` (delivered, read).
fn state_of_mine(s: &mut State, key: &Key, ids: &[String], state: &str) {
    let fp = fp_id(key);
    let hex = wire::hex(key);
    let conv = s.conv(key);
    let mut moved = Vec::new();
    for m in conv.messages.iter_mut().filter(|m| m.mine && ids.contains(&m.id)) {
        if advance(m, state) {
            moved.push(m.id.clone());
        }
    }
    // One of ours older than those shown still has its state on the card.
    for id in ids {
        if moved.contains(id) || conv.has_seen(id) {
            msg_log::chat_note(false, &fp, &ChatEntry::State { dir: Direction::Out, key: hex.clone(),
                                                                id: id.clone(), state: state.into() });
        }
    }
    if !moved.is_empty() {
        changed();
    }
}

/// A contact's reaction: applied once, kept on the card.
fn reacted(s: &mut State, key: &Key, r: ChatReact) {
    let fp = fp_id(key);
    let conv = s.conv(key);
    if conv.has_seen(&r.id) {
        return;
    }
    conv.saw(&r.id);
    if let Some(m) = conv.find(&r.to) {
        react_on(m, false, &r.emoji, r.ts);
    }
    info(&format!("reaction {} from {} on {}", if r.emoji.is_empty() { "removed" } else { "received" }, r.name, r.to));
    msg_log::chat_note(false, &fp, &ChatEntry::React(r));
    changed();
}

/// A message from `key`: shown if a contact's, held if a stranger's.
/// Whether to ack it: false only when it could not be kept.
fn received(s: &mut State, key: &Key, name: &str, m: Message) -> bool {
    let fp = fp_id(key);
    let line = ChatLine {
        dir: Direction::In,
        key: wire::hex(key),
        name: name.to_string(),
        id: m.id.clone(),
        ts: m.ts,
        text: m.text.clone(),
        reply_to: m.reply_to.clone(),
        state: m.state.clone(),
    };
    if let Some(ci) = s.book.find(key) {
        let contact = s.book.contacts[ci].name.clone();
        let conv = s.conv(key);
        if conv.has_seen(&m.id) || conv.find(&m.id).is_some() {
            info(&format!("received {} from {name} again", line.id));
            return true;
        }
        conv.saw(&m.id);
        let text = m.text.clone();
        insert_by_ts(&mut conv.messages, m, SHOWN_MAX);
        msg_log::chat_record(false, &fp, &line);
        info(&format!("received {} from {name}", line.id));
        // On screen, it is read once acked (on_frame); else it alerts.
        if s.viewing != Some(*key) {
            s.book.contacts[ci].unread = s.book.contacts[ci].unread.saturating_add(1);
            save(s);
            msg_alert::arrived(Transport::Tailnet, &fp, &contact, &text);
        }
        changed();
        return true;
    }
    let r = match s.requests.iter().position(|r| &r.key == key) {
        Some(r) => r,
        None if s.requests.len() >= REQUESTS_MAX => {
            warn(&format!("not holding {name}'s message: {REQUESTS_MAX} contact requests wait already"));
            return false;
        }
        None => {
            s.requests.push(Request { key: *key, name: name.to_string(), address: None, held: Vec::new() });
            info(&format!("contact request from {name} {}", wire::fingerprint(key)));
            s.requests.len() - 1
        }
    };
    let req = &mut s.requests[r];
    if req.held.iter().any(|x| x.id == m.id) {
        return true;
    }
    req.name = name.to_string();
    let text = m.text.clone();
    insert_by_ts(&mut req.held, m, HELD_MAX);
    msg_log::chat_record(true, &fp, &line);
    info(&format!("held {} from {name}", line.id));
    msg_alert::arrived(Transport::Tailnet, &fp, name, &text);
    changed();
    true
}

// ---------------------------------------------------------------------------
// The user's actions
// ---------------------------------------------------------------------------

fn with<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    lock().as_mut().map(f)
}

/// Accept the contact request from `key`: pin its key, show what it held,
/// tell it.
pub(crate) fn accept(key: &Key) -> i32 {
    with(|s| {
        let Some(r) = s.requests.iter().position(|r| &r.key == key) else {
            return ESP_ERR_NOT_FOUND;
        };
        let req = s.requests.remove(r);
        let seen = Seen { key, name: &req.name, address: req.address };
        let Some(ci) = s.book.accept(&seen) else {
            s.requests.insert(r, req);
            return ESP_ERR_NO_MEM;
        };
        let fp = fp_id(key);
        msg_log::chat_settle_request(&fp, true);
        let name = s.book.contacts[ci].name.clone();
        // Read back from the card, which has the held messages now.
        s.convs.retain(|c| &c.key != key);
        let conv = s.conv(key);
        for m in req.held {
            if conv.find(&m.id).is_none() {
                conv.saw(&m.id);
                insert_by_ts(&mut conv.messages, m, SHOWN_MAX);
            }
        }
        info(&format!("accepted {name} {}", wire::fingerprint(key)));
        let id = new_id();
        s.queue(key, id.clone(), wire::contact(&id, true));
        changed();
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// Decline the contact request from `key`: drop what it held, tell it.
pub(crate) fn decline(key: &Key) -> i32 {
    with(|s| {
        let Some(r) = s.requests.iter().position(|r| &r.key == key) else {
            return ESP_ERR_NOT_FOUND;
        };
        let req = s.requests.remove(r);
        msg_log::chat_settle_request(&fp_id(key), false);
        info(&format!("declined {} {}", req.name, wire::fingerprint(key)));
        let id = new_id();
        s.queue(key, id.clone(), wire::contact(&id, false));
        changed();
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// Put the key flagged for the contact pinned to `key` in its place.
pub(crate) fn accept_new_key(key: &Key) -> i32 {
    with(|s| {
        let Some(ci) = s.book.find(key) else { return ESP_ERR_NOT_FOUND };
        let Some(new) = s.book.contacts[ci].flagged_key else { return ESP_ERR_INVALID_STATE };
        let Some(old) = s.book.accept_new_key(ci) else { return ESP_ERR_INVALID_STATE };
        // The conversation and what waits carry on under the new key.
        s.convs.retain(|c| c.key != old && c.key != new);
        msg_log::chat_rekey(&fp_id(&old), &fp_id(&new));
        for (k, _) in s.outbox.iter_mut().filter(|(k, _)| k == &old) {
            *k = new;
        }
        let name = s.book.find(&new).map_or(String::new(), |i| s.book.contacts[i].name.clone());
        info(&format!("accepted a new key for {name}: {} in place of {}", wire::fingerprint(&new), wire::fingerprint(&old)));
        save(s);
        changed();
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

pub(crate) fn rename(key: &Key, name: &str) -> i32 {
    with(|s| {
        let Some(ci) = s.book.find(key) else { return ESP_ERR_NOT_FOUND };
        if !s.book.rename(ci, name) {
            return ESP_ERR_INVALID_ARG;
        }
        save(s);
        changed();
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// Unpin a contact: its next message is a contact request again.
pub(crate) fn remove(key: &Key) -> i32 {
    with(|s| {
        let Some(ci) = s.book.find(key) else { return ESP_ERR_NOT_FOUND };
        let c = s.book.remove(ci).unwrap();
        s.convs.retain(|c| &c.key != key);
        s.outbox.retain(|(k, _)| k != key);
        s.retries.retain(|(k, _)| k != key);
        for conn in s.conns.iter_mut().filter(|conn| conn.key() == Some(key)) {
            conn.close(wire::CLOSE_NORMAL, "contact removed");
        }
        info(&format!("removed {} {}", c.name, wire::fingerprint(key)));
        save(s);
        changed();
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// Add a contact by its tailnet address or name and port; its key is
/// pinned when it is first dialled. The key it is known by until then (a
/// placeholder), or its key if it is a contact already.
pub(crate) fn add_contact(host: &str, port: u16, name: &str) -> Result<Key, i32> {
    let host = host.trim();
    if host.is_empty() || port == 0 {
        return Err(ESP_ERR_INVALID_ARG);
    }
    with(|s| {
        let at = Dial { host: host.to_string(), port };
        let i = s.book.add(at, name, random::<32>()).ok_or(ESP_ERR_NO_MEM)?;
        let c = &s.book.contacts[i];
        info(&format!("added {} at {host}:{port}", c.name));
        let key = c.key;
        save(s);
        changed();
        Ok(key)
    })
    .unwrap_or(Err(ESP_ERR_INVALID_STATE))
}

/// Send `text` to contact `key`, as a reply to `reply_to` if given: into
/// the conversation and the outbox, delivered now if it is connected,
/// dialled at once if it is not. The message's id.
pub(crate) fn send_text(key: &Key, text: &str, reply_to: Option<&str>) -> Result<String, i32> {
    if text.trim().is_empty() || text.len() > wire::MAX_TEXT_BYTES {
        return Err(ESP_ERR_INVALID_SIZE);
    }
    let reply_to = reply_to.filter(|r| wire::is_hex(r, 32));
    with(|s| {
        let ci = s.book.find(key).ok_or(ESP_ERR_NOT_FOUND)?;
        let id = new_id();
        let ts = wall_ms();
        let name = s.book.contacts[ci].name.clone();
        let m = Message {
            id: id.clone(),
            mine: true,
            ts,
            text: text.to_string(),
            state: "waiting".into(),
            reply_to: reply_to.map(str::to_string),
            react_me: None,
            react_them: None,
        };
        let line = ChatLine {
            dir: Direction::Out,
            key: wire::hex(key),
            name: name.clone(),
            id: id.clone(),
            ts,
            text: text.to_string(),
            reply_to: m.reply_to.clone(),
            state: m.state.clone(),
        };
        let conv = s.conv(key);
        conv.saw(&id);
        insert_by_ts(&mut conv.messages, m, SHOWN_MAX);
        msg_log::chat_record(false, &fp_id(key), &line);
        s.retry(key).seen_online();
        s.queue(key, id.clone(), wire::msg(&id, ts, text, reply_to));
        info(&format!("sending {id} to {name}"));
        changed();
        Ok(id)
    })
    .unwrap_or(Err(ESP_ERR_INVALID_STATE))
}

/// React to message `to` of contact `key` with `emoji`, "" to remove the
/// reaction: shown at once, delivered like a message. The reaction's id.
pub(crate) fn react(key: &Key, to: &str, emoji: &str) -> Result<String, i32> {
    if emoji.len() > 32 || !wire::is_hex(to, 32) {
        return Err(ESP_ERR_INVALID_ARG);
    }
    with(|s| {
        let ci = s.book.find(key).ok_or(ESP_ERR_NOT_FOUND)?;
        let id = new_id();
        let ts = wall_ms();
        let r = ChatReact {
            dir: Direction::Out,
            key: wire::hex(key),
            name: s.book.contacts[ci].name.clone(),
            id: id.clone(),
            ts,
            to: to.to_string(),
            emoji: emoji.to_string(),
        };
        let conv = s.conv(key);
        conv.saw(&id);
        if let Some(m) = conv.find(to) {
            react_on(m, true, emoji, ts);
        }
        msg_log::chat_note(false, &fp_id(key), &ChatEntry::React(r));
        s.retry(key).seen_online();
        s.queue(key, id.clone(), wire::react(&id, ts, to, emoji));
        changed();
        Ok(id)
    })
    .unwrap_or(Err(ESP_ERR_INVALID_STATE))
}

/// The user has read what contact `key` sent: the ids given, or all.
pub(crate) fn mark_read(key: &Key, ids: Option<&[String]>) -> i32 {
    with(|s| {
        if s.book.find(key).is_none() {
            return ESP_ERR_NOT_FOUND;
        }
        s.mark_read(key, ids);
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// Tell contact `key` the user is typing, or stopped: sent if it is
/// connected and takes it, never queued. The app repeats `on` every 4 s
/// while typing goes on.
pub(crate) fn set_typing(key: &Key, on: bool) -> i32 {
    with(|s| match s.open_conn(key) {
        Some(c) if c.may("typing") => {
            c.send_text(&wire::typing(on));
            ESP_OK
        }
        _ => ESP_ERR_INVALID_STATE,
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

pub(crate) fn is_typing(key: &Key) -> bool {
    with(|s| s.typing.iter().any(|(k, t)| k == key && policy::is_typing(Some(*t), now_ms()))).unwrap_or(false)
}

pub(crate) fn read_receipts() -> bool {
    with(|s| s.settings.read_receipts).unwrap_or(true)
}

/// Whether the user's reading sends read receipts; remembered in
/// system.json (thistle_os.tailnet_chat.read_receipts).
pub(crate) fn set_read_receipts(on: bool) -> i32 {
    with(|s| s.settings.read_receipts = on);
    info(&format!("read receipts {}", if on { "on" } else { "off" }));
    changed();
    crate::system_json::save(SECTION, "read_receipts", serde_json::Value::Bool(on))
}

/// Look for `key` now, or every contact, as when it is seen online:
/// whatever waits for it is dialled for at once.
pub(crate) fn flush(key: Option<&Key>) -> i32 {
    with(|s| {
        for (k, r) in s.retries.iter_mut() {
            if key.is_none_or(|key| key == k) {
                r.seen_online();
            }
        }
        ESP_OK
    })
    .unwrap_or(ESP_ERR_INVALID_STATE)
}

/// The conversation the app has on screen, or none: its messages are read
/// as they arrive, and alert nothing.
pub(crate) fn set_viewing(key: Option<&Key>) {
    with(|s| {
        s.viewing = key.copied();
        if let Some(k) = key {
            if s.book.find(k).is_some() {
                s.mark_read(k, None);
            }
        }
    });
    msg_alert_viewing(key);
}

// ---------------------------------------------------------------------------
// What the app reads
// ---------------------------------------------------------------------------

pub(crate) struct ContactView {
    pub name: String,
    pub key: Key,
    pub seen_name: String,
    pub flagged_key: Option<Key>,
    pub accepted_by_them: Option<bool>,
    pub connected: bool,
    pub pending: bool,
    pub dial: Option<Dial>,
    pub unread: u32,
    pub waiting: u32,
}

pub(crate) fn contacts() -> Vec<ContactView> {
    with(|s| {
        s.book
            .contacts
            .iter()
            .map(|c| ContactView {
                name: c.name.clone(),
                key: c.key,
                seen_name: c.seen_name.clone(),
                flagged_key: c.flagged_key,
                accepted_by_them: c.accepted_by_them,
                connected: s.connected(&c.key),
                pending: c.pending,
                dial: c.dial.clone(),
                unread: c.unread,
                waiting: s.outbox.iter().find(|(k, _)| k == &c.key).map_or(0, |(_, f)| f.len() as u32),
            })
            .collect()
    })
    .unwrap_or_default()
}

/// (key, name, held messages)
pub(crate) fn requests() -> Vec<(Key, String, Vec<Message>)> {
    with(|s| s.requests.iter().map(|r| (r.key, r.name.clone(), r.held.clone())).collect()).unwrap_or_default()
}

/// A contact's latest messages, oldest first; none for a stranger.
pub(crate) fn messages(key: &Key) -> Vec<Message> {
    with(|s| if s.book.find(key).is_some() { s.conv(key).messages.clone() } else { Vec::new() }).unwrap_or_default()
}

/// The ids of the messages and reactions waiting for `key`'s ack.
pub(crate) fn waiting(key: &Key) -> Vec<String> {
    with(|s| {
        s.outbox
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, f)| f.iter().filter(|(_, fr)| !fr.contains(r#""type":"contact""#)).map(|(id, _)| id.clone()).collect())
            .unwrap_or_default()
    })
    .unwrap_or_default()
}

/// Messages received from contacts and not yet read.
pub(crate) fn unread() -> u32 {
    with(|s| s.unread()).unwrap_or(0)
}

/// (shown, why, own key, port listened on, own name)
fn status() -> Option<(Shown, String, Option<Key>, u16, String, usize)> {
    with(|s| {
        (s.shown, s.why.clone(), s.identity.as_ref().map(own_key),
         s.listening_on.map_or(0, |a| a.port()), s.name.clone(),
         s.conns.iter().filter(|c| matches!(c.phase, Phase::Open { .. }) && !c.closing).count())
    })
}

// ---------------------------------------------------------------------------
// FFI — thistle/tailnet_chat.h
// ---------------------------------------------------------------------------

/// tailnet_chat_status_t
#[repr(C)]
pub struct CStatus {
    pub state: u32,
    pub why: [u8; 96],
    pub port: u16,
    pub key: [u8; 65],
    pub fingerprint: [u8; 25],
    pub name: [u8; 64],
    pub contacts: u16,
    pub requests: u16,
    pub connections: u16,
    pub generation: u32,
    pub read_receipts: bool,
    pub unread: u32,
}

/// tailnet_chat_contact_t
#[repr(C)]
pub struct CContact {
    pub key: [u8; 65],
    pub fingerprint: [u8; 25],
    pub name: [u8; 65],
    pub seen_name: [u8; 65],
    pub flagged: bool,
    pub flagged_fingerprint: [u8; 25],
    pub connected: bool,
    /// -1 not known, 0 declined, 1 accepted
    pub accepted_by_them: i8,
    pub pending: bool,
    pub unread: u16,
    pub waiting: u16,
    pub typing: bool,
}

/// tailnet_chat_request_t
#[repr(C)]
pub struct CRequest {
    pub key: [u8; 65],
    pub fingerprint: [u8; 25],
    pub name: [u8; 65],
    pub held: u16,
    pub preview: [u8; 96],
}

/// tailnet_chat_message_t
#[repr(C)]
pub struct CMessage {
    pub id: [u8; 33],
    pub mine: bool,
    pub ts: u64,
    pub state: [u8; 12],
    pub reply_to: [u8; 33],
    pub react_me: [u8; 33],
    pub react_them: [u8; 33],
}

fn copy_str(dst: &mut [u8], src: &str) {
    let src = wire::cut(src, dst.len().saturating_sub(1));
    dst[..src.len()].copy_from_slice(src.as_bytes());
    dst[src.len()..].fill(0);
}

unsafe fn key_arg(key: *const c_char) -> Option<Key> {
    if key.is_null() {
        return None;
    }
    wire::unhex::<32>(unsafe { CStr::from_ptr(key) }.to_str().ok()?)
}

unsafe fn str_arg<'a>(s: *const c_char) -> Option<&'a str> {
    if s.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(s) }.to_str().ok()
}

#[no_mangle]
pub extern "C" fn tailnet_chat_generation() -> u32 {
    GENERATION.load(Ordering::Relaxed)
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_chat_status_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_status(out: *mut CStatus) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let Some((shown, why, key, port, name, conns)) = status() else { return ESP_ERR_INVALID_STATE };
    out.state = if crate::tailnet_manager::tailnet_available() { shown as u32 } else { Shown::Unavailable as u32 };
    copy_str(&mut out.why, &why);
    out.port = port;
    copy_str(&mut out.key, &key.map(|k| wire::hex(&k)).unwrap_or_default());
    copy_str(&mut out.fingerprint, &key.map(|k| wire::fingerprint(&k)).unwrap_or_default());
    copy_str(&mut out.name, &name);
    out.contacts = with(|s| s.book.contacts.len()).unwrap_or(0) as u16;
    out.requests = with(|s| s.requests.len()).unwrap_or(0) as u16;
    out.connections = conns as u16;
    out.generation = tailnet_chat_generation();
    out.read_receipts = read_receipts();
    out.unread = unread();
    ESP_OK
}

#[no_mangle]
pub extern "C" fn tailnet_chat_contact_count() -> i32 {
    with(|s| s.book.contacts.len() as i32).unwrap_or(0)
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_chat_contact_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_contact(index: i32, out: *mut CContact) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let Some(c) = usize::try_from(index).ok().and_then(|i| contacts().into_iter().nth(i)) else {
        return ESP_ERR_INVALID_ARG;
    };
    copy_str(&mut out.key, &wire::hex(&c.key));
    copy_str(&mut out.fingerprint, &if c.pending { String::new() } else { wire::fingerprint(&c.key) });
    copy_str(&mut out.name, &c.name);
    copy_str(&mut out.seen_name, &c.seen_name);
    out.flagged = c.flagged_key.is_some();
    copy_str(&mut out.flagged_fingerprint, &c.flagged_key.map(|k| wire::fingerprint(&k)).unwrap_or_default());
    out.connected = c.connected;
    out.accepted_by_them = c.accepted_by_them.map_or(-1, |a| a as i8);
    out.pending = c.pending;
    out.unread = c.unread.min(u16::MAX as u32) as u16;
    out.waiting = c.waiting.min(u16::MAX as u32) as u16;
    out.typing = is_typing(&c.key);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn tailnet_chat_request_count() -> i32 {
    with(|s| s.requests.len() as i32).unwrap_or(0)
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_chat_request_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_request(index: i32, out: *mut CRequest) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let Some((key, name, held)) = usize::try_from(index).ok().and_then(|i| requests().into_iter().nth(i)) else {
        return ESP_ERR_INVALID_ARG;
    };
    copy_str(&mut out.key, &wire::hex(&key));
    copy_str(&mut out.fingerprint, &wire::fingerprint(&key));
    copy_str(&mut out.name, &name);
    out.held = held.len() as u16;
    let preview = held.last().map(|m| m.text.replace('\n', " ")).unwrap_or_default();
    copy_str(&mut out.preview, &preview);
    ESP_OK
}

/// # Safety
/// `key` must be null or a NUL-terminated string, `text` null or
/// `text_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_held(key: *const c_char, index: i32, text: *mut c_char,
                                               text_len: usize) -> i32 {
    let Some(key) = (unsafe { key_arg(key) }) else { return ESP_ERR_INVALID_ARG };
    let Some(m) = requests()
        .into_iter()
        .find(|(k, _, _)| k == &key)
        .and_then(|(_, _, held)| usize::try_from(index).ok().and_then(|i| held.into_iter().nth(i)))
    else {
        return ESP_ERR_INVALID_ARG;
    };
    if !text.is_null() && text_len > 0 {
        copy_str(unsafe { std::slice::from_raw_parts_mut(text as *mut u8, text_len) }, &m.text);
    }
    ESP_OK
}

/// # Safety
/// `key` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_message_count(key: *const c_char) -> i32 {
    unsafe { key_arg(key) }.map_or(0, |k| messages(&k).len() as i32)
}

/// # Safety
/// `key` must be null or a NUL-terminated string, `out` null or writable,
/// `text` null or `text_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_message(key: *const c_char, index: i32, out: *mut CMessage,
                                                  text: *mut c_char, text_len: usize) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let Some(key) = (unsafe { key_arg(key) }) else { return ESP_ERR_INVALID_ARG };
    let Some(m) = usize::try_from(index).ok().and_then(|i| messages(&key).into_iter().nth(i)) else {
        return ESP_ERR_INVALID_ARG;
    };
    copy_str(&mut out.id, &m.id);
    out.mine = m.mine;
    out.ts = m.ts;
    copy_str(&mut out.state, &m.state);
    copy_str(&mut out.reply_to, m.reply_to.as_deref().unwrap_or(""));
    copy_str(&mut out.react_me, Message::shown(&m.react_me).unwrap_or(""));
    copy_str(&mut out.react_them, Message::shown(&m.react_them).unwrap_or(""));
    if !text.is_null() && text_len > 0 {
        let dst = unsafe { std::slice::from_raw_parts_mut(text as *mut u8, text_len) };
        copy_str(dst, &m.text);
    }
    ESP_OK
}

/// The text of message `id` of contact `key`, if it is among those kept:
/// what a reply quotes.
///
/// # Safety
/// `key` and `id` must be null or NUL-terminated strings, `text` null or
/// `text_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_find_message(key: *const c_char, id: *const c_char, mine: *mut bool,
                                                   text: *mut c_char, text_len: usize) -> i32 {
    let (Some(key), Some(id)) = (unsafe { key_arg(key) }, unsafe { str_arg(id) }) else {
        return ESP_ERR_INVALID_ARG;
    };
    let Some(m) = messages(&key).into_iter().find(|m| m.id == id) else { return ESP_ERR_NOT_FOUND };
    if let Some(mine) = unsafe { mine.as_mut() } {
        *mine = m.mine;
    }
    if !text.is_null() && text_len > 0 {
        copy_str(unsafe { std::slice::from_raw_parts_mut(text as *mut u8, text_len) }, &m.text);
    }
    ESP_OK
}

macro_rules! by_key {
    ($name:ident, $f:path) => {
        /// # Safety
        /// `key` must be null or a NUL-terminated string.
        #[no_mangle]
        pub unsafe extern "C" fn $name(key: *const c_char) -> i32 {
            match unsafe { key_arg(key) } {
                Some(k) => $f(&k),
                None => ESP_ERR_INVALID_ARG,
            }
        }
    };
}

by_key!(tailnet_chat_accept, accept);
by_key!(tailnet_chat_decline, decline);
by_key!(tailnet_chat_accept_new_key, accept_new_key);
by_key!(tailnet_chat_remove, remove);

/// # Safety
/// `key` and `name` must be null or NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_rename(key: *const c_char, name: *const c_char) -> i32 {
    let (Some(k), false) = (unsafe { key_arg(key) }, name.is_null()) else { return ESP_ERR_INVALID_ARG };
    match unsafe { CStr::from_ptr(name) }.to_str() {
        Ok(n) => rename(&k, n),
        Err(_) => ESP_ERR_INVALID_ARG,
    }
}

/// # Safety
/// `key`, `text` and `reply_to` must be null or NUL-terminated strings;
/// `id_out` null or 33 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_send_text(key: *const c_char, text: *const c_char, reply_to: *const c_char,
                                                id_out: *mut c_char) -> i32 {
    let (Some(k), Some(text)) = (unsafe { key_arg(key) }, unsafe { str_arg(text) }) else {
        return ESP_ERR_INVALID_ARG;
    };
    let reply_to = unsafe { str_arg(reply_to) }.filter(|r| !r.is_empty());
    match send_text(&k, text, reply_to) {
        Ok(id) => {
            if !id_out.is_null() {
                copy_str(unsafe { std::slice::from_raw_parts_mut(id_out as *mut u8, 33) }, &id);
            }
            ESP_OK
        }
        Err(e) => e,
    }
}

/// # Safety
/// `key`, `message` and `emoji` must be null or NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_react(key: *const c_char, message: *const c_char, emoji: *const c_char) -> i32 {
    let (Some(k), Some(to)) = (unsafe { key_arg(key) }, unsafe { str_arg(message) }) else {
        return ESP_ERR_INVALID_ARG;
    };
    match react(&k, to, unsafe { str_arg(emoji) }.unwrap_or("")) {
        Ok(_) => ESP_OK,
        Err(e) => e,
    }
}

/// # Safety
/// `key` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_mark_read(key: *const c_char) -> i32 {
    match unsafe { key_arg(key) } {
        Some(k) => mark_read(&k, None),
        None => ESP_ERR_INVALID_ARG,
    }
}

/// # Safety
/// `key` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_set_typing(key: *const c_char, on: bool) -> i32 {
    match unsafe { key_arg(key) } {
        Some(k) => set_typing(&k, on),
        None => ESP_ERR_INVALID_ARG,
    }
}

/// # Safety
/// `key` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_is_typing(key: *const c_char) -> bool {
    unsafe { key_arg(key) }.is_some_and(|k| is_typing(&k))
}

/// # Safety
/// `key` must be null or a NUL-terminated string: none on screen.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_set_viewing(key: *const c_char) {
    set_viewing(unsafe { key_arg(key) }.as_ref());
}

#[no_mangle]
pub extern "C" fn tailnet_chat_read_receipts() -> bool {
    read_receipts()
}

#[no_mangle]
pub extern "C" fn tailnet_chat_set_read_receipts(on: bool) -> i32 {
    set_read_receipts(on)
}

/// Messages received from contacts and not yet read, for the status bar's
/// envelope and Home's row.
#[no_mangle]
pub extern "C" fn tailnet_chat_unread_count() -> u32 {
    unread()
}

/// # Safety
/// `host` and `name` must be null or NUL-terminated strings, `key_out` null
/// or 65 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_add_contact(host: *const c_char, port: u16, name: *const c_char,
                                                  key_out: *mut c_char) -> i32 {
    let Some(host) = (unsafe { str_arg(host) }) else { return ESP_ERR_INVALID_ARG };
    match add_contact(host, if port == 0 { wire::PORT } else { port }, unsafe { str_arg(name) }.unwrap_or("")) {
        Ok(k) => {
            if !key_out.is_null() {
                copy_str(unsafe { std::slice::from_raw_parts_mut(key_out as *mut u8, 65) }, &wire::hex(&k));
            }
            ESP_OK
        }
        Err(e) => e,
    }
}

// ---------------------------------------------------------------------------
// The simulator's control port, for the conformance suite
// ---------------------------------------------------------------------------

/// THISTLE_SIM_CHAT_CONTROL=PORT: a socket on 127.0.0.1:PORT speaking the
/// control protocol of docs/tailnet-chat/README.md, one JSON object a line,
/// for the user's actions and what the app shows: the conformance suite's
/// simulator target (simulator/tests/test_tailchat.py). It calls the
/// same functions the app's C API does. Simulator builds only.
#[cfg(not(target_os = "espidf"))]
mod sim_control {
    use super::*;
    use serde_json::{json, Value};
    use std::io::{BufRead, BufReader};

    pub(super) fn start() {
        let Some(port) = std::env::var("THISTLE_SIM_CHAT_CONTROL").ok().and_then(|p| p.parse::<u16>().ok()) else {
            return;
        };
        let listener = match TcpListener::bind(("127.0.0.1", port)) {
            Ok(l) => l,
            Err(e) => {
                warn(&format!("control port {port}: {e}"));
                return;
            }
        };
        info(&format!("control port on 127.0.0.1:{}", listener.local_addr().map_or(port, |a| a.port())));
        std::thread::spawn(move || {
            for sock in listener.incoming().flatten() {
                std::thread::spawn(move || serve(sock));
            }
        });
    }

    fn serve(sock: TcpStream) {
        let Ok(mut out) = sock.try_clone() else { return };
        // The first line once listening: who this device is, and where.
        loop {
            if let Some((Shown::Listening, _, Some(key), port, name, _)) = status() {
                let ready = json!({"ready": {"key": wire::hex(&key), "fingerprint": wire::fingerprint(&key),
                                             "name": name, "address": ["127.0.0.1", port]}});
                if writeln!(out, "{ready}").is_err() {
                    return;
                }
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        for line in BufReader::new(sock).lines() {
            let Ok(line) = line else { return };
            let Ok(cmd) = serde_json::from_str::<Value>(&line) else { continue };
            let seq = cmd.get("seq").cloned().unwrap_or(Value::Null);
            let reply = match run(&cmd) {
                Ok(v) => json!({"ok": v, "seq": seq}),
                Err(e) => json!({"error": e, "seq": seq}),
            };
            // The UI's log lines (ESP_LOGI) are printf'd unflushed: a test
            // reading the log after a command sees them.
            unsafe { fflush(std::ptr::null_mut()) };
            if writeln!(out, "{reply}").is_err() {
                return;
            }
        }
    }

    extern "C" {
        fn fflush(stream: *mut c_void) -> i32;
    }

    fn key_of(v: Option<&Value>) -> Result<Key, String> {
        v.and_then(Value::as_str).and_then(wire::unhex::<32>).ok_or_else(|| "no such key".to_string())
    }

    /// A contact by key, name, or [host, port].
    fn contact_key(v: Option<&Value>) -> Result<Key, String> {
        if let Some([host, port]) = v.and_then(Value::as_array).map(Vec::as_slice) {
            let at = Dial { host: host.as_str().unwrap_or("").to_string(), port: port.as_u64().unwrap_or(0) as u16 };
            return with(|s| s.book.by_dial(&at).map(|i| s.book.contacts[i].key))
                .flatten()
                .ok_or_else(|| format!("no contact at {}:{}", at.host, at.port));
        }
        let who = v.and_then(Value::as_str).unwrap_or("");
        if let Some(k) = wire::unhex::<32>(who) {
            return Ok(k);
        }
        contacts().into_iter().find(|c| c.name == who || c.seen_name == who).map(|c| c.key)
            .ok_or_else(|| format!("no contact {who}"))
    }

    fn message_json(m: &Message) -> Value {
        let mut v = json!({"id": m.id, "mine": m.mine, "ts": m.ts, "text": m.text, "state": m.state});
        if let Some(r) = &m.reply_to {
            v["reply_to"] = r.as_str().into();
        }
        let mut reactions = serde_json::Map::new();
        if let Some(e) = Message::shown(&m.react_me) {
            reactions.insert("me".into(), e.into());
        }
        if let Some(e) = Message::shown(&m.react_them) {
            reactions.insert("them".into(), e.into());
        }
        if !reactions.is_empty() {
            v["reactions"] = Value::Object(reactions);
        }
        v
    }

    fn done(rc: i32) -> Result<Value, String> {
        if rc == ESP_OK { Ok(Value::Bool(true)) } else { Err(format!("0x{rc:x}")) }
    }

    fn run(cmd: &Value) -> Result<Value, String> {
        let name = cmd.get("cmd").and_then(Value::as_str).unwrap_or("");
        let text = |k: &str| cmd.get(k).and_then(Value::as_str);
        match name {
            "identity" => {
                let (_, _, key, port, name, _) = status().ok_or("not started")?;
                let key = key.ok_or("no identity")?;
                Ok(json!({"key": wire::hex(&key), "fingerprint": wire::fingerprint(&key), "name": name,
                          "address": ["127.0.0.1", port]}))
            }
            "contacts" => Ok(Value::Array(
                contacts()
                    .iter()
                    .map(|c| {
                        json!({"name": c.name, "key": if c.pending { Value::Null } else { wire::hex(&c.key).into() },
                               "tailnet_name": c.seen_name,
                               "address": c.dial.as_ref().map(|d| json!([d.host, d.port])),
                               "fingerprint": wire::fingerprint(&c.key),
                               "flagged_key": c.flagged_key.map(|k| wire::hex(&k)),
                               "accepted_by_them": c.accepted_by_them,
                               "declined_by_them": c.accepted_by_them == Some(false),
                               "connected": c.connected, "unread": c.unread})
                    })
                    .collect(),
            )),
            "requests" => Ok(Value::Array(
                requests()
                    .iter()
                    .map(|(k, name, held)| {
                        json!({"key": wire::hex(k), "name": name, "fingerprint": wire::fingerprint(k),
                               "held": held.iter().map(message_json).collect::<Vec<_>>()})
                    })
                    .collect(),
            )),
            "accept" => done(accept(&key_of(cmd.get("key"))?)),
            "decline" => done(decline(&key_of(cmd.get("key"))?)),
            "accept_new_key" => done(accept_new_key(&contact_key(cmd.get("contact"))?)),
            "rename" => done(rename(&contact_key(cmd.get("contact"))?, text("name").unwrap_or(""))),
            "remove" | "remove_contact" => done(remove(&contact_key(cmd.get("contact"))?)),
            "add_contact" => {
                let a = cmd.get("address").and_then(Value::as_array).ok_or("no address")?;
                let host = a.first().and_then(Value::as_str).ok_or("no host")?;
                let port = a.get(1).and_then(Value::as_u64).unwrap_or(wire::PORT as u64) as u16;
                let key = add_contact(host, port, text("name").unwrap_or("")).map_err(|e| format!("0x{e:x}"))?;
                let name = contacts().into_iter().find(|c| c.key == key).map(|c| c.name).unwrap_or_default();
                Ok(json!({"name": name}))
            }
            "messages" => {
                let Ok(k) = contact_key(cmd.get("to")) else { return Ok(Value::Array(Vec::new())) };
                Ok(Value::Array(messages(&k).iter().map(message_json).collect()))
            }
            "waiting" => {
                let Ok(k) = contact_key(cmd.get("to")) else { return Ok(Value::Array(Vec::new())) };
                Ok(json!(waiting(&k)))
            }
            "send_text" => {
                let k = contact_key(cmd.get("to"))?;
                send_text(&k, text("text").unwrap_or(""), text("reply_to")).map(Value::from).map_err(|e| format!("0x{e:x}"))
            }
            "react" => {
                let k = contact_key(cmd.get("to"))?;
                react(&k, text("message").unwrap_or(""), text("emoji").unwrap_or(""))
                    .map(Value::from)
                    .map_err(|e| format!("0x{e:x}"))
            }
            "mark_read" => {
                let k = contact_key(cmd.get("to"))?;
                let ids: Option<Vec<String>> = cmd
                    .get("ids")
                    .and_then(Value::as_array)
                    .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect());
                done(mark_read(&k, ids.as_deref()))
            }
            "typing" => {
                let k = contact_key(cmd.get("to"))?;
                done(set_typing(&k, cmd.get("on").and_then(Value::as_bool).unwrap_or(false)))
            }
            "is_typing" => Ok(Value::Bool(key_of(cmd.get("key")).is_ok_and(|k| is_typing(&k)))),
            "set" => {
                if let Some(on) = cmd.get("read_receipts").and_then(Value::as_bool) {
                    set_read_receipts(on);
                }
                Ok(Value::Bool(true))
            }
            "flush" => match cmd.get("to") {
                None | Some(Value::Null) => done(flush(None)),
                to => done(flush(Some(&contact_key(to)?))),
            },
            "viewing" => {
                let k = match cmd.get("to") {
                    None | Some(Value::Null) => None,
                    to => Some(contact_key(to)?),
                };
                set_viewing(k.as_ref());
                Ok(Value::Bool(true))
            }
            "unread" => Ok(json!(unread())),
            "stop" => Ok(Value::Bool(true)),
            other => Err(format!("{other}: not on this device yet")),
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_default_to_the_spec() {
        assert_eq!(settings_in(None), Settings { port: 4810, check_minutes: 15, read_receipts: true });
        let s = settings_in(Some(r#"{"thistle_os":{"tailnet_chat":{"port":5000,"check_minutes":0,"read_receipts":false}}}"#));
        assert_eq!(s, Settings { port: 5000, check_minutes: 0, read_receipts: false });
        assert_eq!(settings_in(Some(r#"{"thistle_os":{"tailnet_chat":{"port":0}}}"#)).port, 4810);
    }

    #[test]
    fn a_dialler_s_signature_verifies_against_its_transcript() {
        let h: serde_json::Value =
            serde_json::from_str(include_str!("../../../docs/tailnet-chat/vectors/handshake.json")).unwrap();
        let key = wire::unhex::<32>(h["dialler"]["key"].as_str().unwrap()).unwrap();
        let t = wire::unhex::<149>(h["transcripts"]["dialler"].as_str().unwrap()).unwrap();
        let auth = wire::check(h["dialler"]["auth"].as_str().unwrap().as_bytes()).unwrap();
        let sig = wire::unhex::<64>(wire::str_of(&auth, "sig")).unwrap();
        assert!(verify(&key, &t, &sig));
        let mut bad = sig;
        bad[0] ^= 1;
        assert!(!verify(&key, &t, &bad));
    }

    fn m(id: &str, ts: u64) -> Message {
        Message { id: id.into(), mine: false, ts, text: id.into(), state: "received".into(), reply_to: None,
                  react_me: None, react_them: None }
    }

    #[test]
    fn messages_are_kept_in_the_order_written_and_the_latest_kept() {
        let mut list = Vec::new();
        insert_by_ts(&mut list, m("b", 20), 3);
        insert_by_ts(&mut list, m("a", 10), 3);
        insert_by_ts(&mut list, m("c", 30), 3);
        insert_by_ts(&mut list, m("c2", 30), 3);
        let ids: Vec<&str> = list.iter().map(|x| x.id.as_str()).collect();
        assert_eq!(ids, ["b", "c", "c2"]);
    }

    fn line(dir: Direction, id: &str, ts: u64, state: &str) -> ChatEntry {
        ChatEntry::Message(ChatLine { dir, key: "ab".repeat(32), name: "laptop".into(), id: id.into(), ts,
                                      text: format!("text {id}"), reply_to: None, state: state.into() })
    }

    fn state(dir: Direction, id: &str, s: &str) -> ChatEntry {
        ChatEntry::State { dir, key: "ab".repeat(32), id: id.into(), state: s.into() }
    }

    fn reaction(dir: Direction, id: &str, ts: u64, to: &str, emoji: &str) -> ChatEntry {
        ChatEntry::React(ChatReact { dir, key: "ab".repeat(32), name: "laptop".into(), id: id.into(), ts,
                                     to: to.into(), emoji: emoji.into() })
    }

    #[test]
    fn a_conversation_read_back_from_its_file_has_each_message_as_it_ended() {
        use Direction::{In, Out};
        let entries = vec![
            line(Out, "mine", 20, "waiting"),
            line(In, "late", 10, "received"), // written first, arrived second
            state(Out, "mine", "read"),
            state(Out, "mine", "delivered"), // an ack after the read changes nothing
            state(In, "late", "read"),
            reaction(In, "r1", 30, "mine", "👍"),
            reaction(In, "r2", 40, "mine", "🎉"),
            reaction(In, "r0", 25, "mine", "😐"), // older, delivered late
            reaction(Out, "r3", 50, "late", "❤"),
            reaction(Out, "r4", 60, "late", ""),
            line(In, "late", 10, "received"), // the same message twice
        ];
        let conv = fold([1; 32], entries, 20);
        let shown: Vec<(&str, &str)> = conv.messages.iter().map(|m| (m.id.as_str(), m.state.as_str())).collect();
        assert_eq!(shown, [("late", "read"), ("mine", "read")]);
        assert_eq!(Message::shown(&conv.messages[1].react_them), Some("🎉"));
        assert_eq!(Message::shown(&conv.messages[0].react_me), None, "removed");
        for id in ["mine", "late", "r0", "r1", "r4"] {
            assert!(conv.has_seen(id), "{id}");
        }
        assert!(!conv.has_seen("never"));
    }

    #[test]
    fn a_long_conversation_keeps_its_latest_messages_and_remembers_more_ids() {
        let entries: Vec<ChatEntry> = (0..50).map(|i| line(Direction::In, &format!("m{i}"), i, "received")).collect();
        let conv = fold([1; 32], entries, SHOWN_MAX);
        assert_eq!(conv.messages.len(), SHOWN_MAX);
        assert_eq!(conv.messages[0].id, "m30");
        assert!(conv.has_seen("m0"), "an id older than those shown is still known");
    }
}
