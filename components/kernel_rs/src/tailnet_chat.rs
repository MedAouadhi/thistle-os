// SPDX-License-Identifier: BSD-3-Clause
//! Tailnet chat: this device as a chat peer on the user's tailnet
//! (ADR-0015, docs/tailnet-chat/protocol.md).
//!
//! - While the tailnet is connected, a listener on the tailnet address,
//!   port `thistle_os.tailnet_chat.port` (4810), takes WebSockets from chat
//!   peers. Only the tailnet's addresses are admitted (and loopback in the
//!   simulator), at most CONNECTIONS_MAX at once.
//! - Nothing here has a thread: the sockets are non-blocking and a kernel
//!   worker job polls them every POLL_MS (`tick`). Connection buffers are
//!   allocated large enough to land in PSRAM.
//! - The device proves its MeshCore identity (`/spiffs/config/mesh_identity`)
//!   in the handshake, signing through the kernel's crypto.
//! - Who a proven key is, and what accepting, renaming and removing do, is
//!   decided in tailnet_chat_policy.rs; the wire is tailnet_chat_wire.rs.
//! - Contacts, their pinned keys and names, and what waits for their ack
//!   are kept in this module's own file, `/spiffs/tailnet_chat.json`.
//! - A stranger's messages are acked and held as a contact request; held
//!   and accepted messages go into the message history as its third kind
//!   (msg_log.rs, `messages/tailnet/`).
//!
//! The app (apps/tailchat) only reads and asks, through thistle/tailnet_chat.h.

use std::ffi::{c_char, CStr, CString};
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::os::raw::c_void;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use crate::msg_log::{self, ChatLine, Direction};
use crate::tailnet_chat_policy::{self as policy, Book, Outbox, Seen, Trust};
use crate::tailnet_chat_wire::{self as wire, ErrorFrame, Key, Role, Upgrade, WsEvent, WsReader};

const SECTION: &str = "tailnet_chat";

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NO_MEM: i32 = 0x101;

/// How often the sockets are polled.
const POLL_MS: u64 = 100;
/// The most chat connections open at once (the spec's budget of sockets).
pub(crate) const CONNECTIONS_MAX: usize = 4;
const HANDSHAKE_MS: u64 = 10_000;
const PING_MS: u64 = 20_000;
const IDLE_MS: u64 = 120_000;
/// Unsent bytes past which a connection is given up as stuck.
const TX_MAX: usize = 64 * 1024;
/// Messages kept per conversation for the app; the rest are on the card.
pub(crate) const SHOWN_MAX: usize = 20;
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
// State
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

/// One message as the app shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Message {
    pub id: String,
    pub mine: bool,
    pub ts: u64,
    pub text: String,
    pub state: String,
}

impl Message {
    fn from_line(l: ChatLine) -> Message {
        Message { id: l.id, mine: l.dir == Direction::Out, ts: l.ts, text: l.text, state: l.state }
    }
}

/// A contact request: a stranger and what it sent, held.
pub(crate) struct Request {
    pub key: Key,
    pub name: String,
    pub address: Option<IpAddr>,
    pub held: Vec<Message>,
}

#[derive(Debug)]
enum Phase {
    /// Reading the HTTP upgrade.
    Upgrade,
    /// Our hello sent; waiting for theirs.
    Hello,
    /// Their hello read; waiting for the dialler's auth.
    Auth { key: Key, nonce: [u8; 32], name: String },
    /// Authenticated as `key`.
    Open { key: Key, name: String },
}

struct Conn {
    sock: TcpStream,
    peer: SocketAddr,
    rx: Vec<u8>,
    ws: WsReader,
    tx: Vec<u8>,
    phase: Phase,
    nonce: [u8; 32],
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

    fn send_text(&mut self, text: &str) {
        self.tx.extend_from_slice(&wire::ws_frame(wire::OP_TEXT, text.as_bytes(), None));
    }

    fn send_error(&mut self, e: &ErrorFrame) {
        self.send_text(&e.to_json());
    }

    fn close(&mut self, code: u16, why: &str) {
        if !self.closing {
            if !matches!(self.phase, Phase::Upgrade) {
                self.tx.extend_from_slice(&wire::ws_close(code, None));
            }
            self.closing = true;
            let who = match &self.phase {
                Phase::Open { name, key } => format!("{name} ({})", wire::fingerprint(key)),
                _ => self.peer.ip().to_string(),
            };
            info(&format!("closed {who}: {why} ({code})"));
        }
    }
}

struct State {
    settings: Settings,
    identity: Option<Identity>,
    name: String,
    listener: Option<TcpListener>,
    listening_on: Option<SocketAddr>,
    conns: Vec<Conn>,
    book: Book,
    outbox: Outbox,
    requests: Vec<Request>,
    /// The latest messages of each contact, read from the card the first
    /// time they are wanted.
    convs: Vec<(Key, Vec<Message>)>,
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
            requests: Vec::new(),
            convs: Vec::new(),
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

    fn conv(&mut self, key: &Key) -> &mut Vec<Message> {
        let i = match self.convs.iter().position(|(k, _)| k == key) {
            Some(i) => i,
            None => {
                let fp = wire::fingerprint_id(key);
                let read = msg_log::chat_read(msg_log::root(), false, &fp, SHOWN_MAX);
                self.convs.push((*key, read.into_iter().map(Message::from_line).collect()));
                self.convs.len() - 1
            }
        };
        &mut self.convs[i].1
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
        if let Some(c) = self.conns.iter_mut().find(|c| !c.closing && c.key() == Some(key)) {
            for (id, frame) in frames {
                if !c.sent.contains(&id) {
                    c.send_text(&frame);
                    c.sent.push(id);
                }
            }
        }
    }

    fn connected(&self, key: &Key) -> bool {
        self.conns.iter().any(|c| !c.closing && c.key() == Some(key))
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
        if let Err(e) = listen(s, &ip) {
            shown = Shown::NoPort;
            why = format!("Could not listen on port {}: {e}", s.settings.port);
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
            c.send_error(&ErrorFrame::new("bad_request").detail("handshake timeout"));
            c.close(wire::CLOSE_HANDSHAKE, "no handshake within 10 s");
        } else if matches!(c.phase, Phase::Open { .. }) {
            if now - c.last_ms > IDLE_MS {
                c.close(wire::CLOSE_NORMAL, "idle");
            } else if now - c.pinged_ms > PING_MS {
                c.pinged_ms = now;
                c.tx.extend_from_slice(&wire::ws_frame(wire::OP_PING, b"", None));
            }
        }
    }
    let before = s.conns.len();
    s.conns.retain_mut(|c| {
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
    });
    if s.conns.len() != before {
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
        if s.conns.len() >= CONNECTIONS_MAX {
            info(&format!("refused {}: {CONNECTIONS_MAX} connections open already", peer.ip()));
            continue;
        }
        if sock.set_nonblocking(true).is_err() {
            continue;
        }
        let _ = sock.set_nodelay(true);
        s.conns.push(Conn {
            sock,
            peer,
            rx: Vec::new(),
            ws: WsReader::with_capacity(true, PSRAM_BUF),
            tx: Vec::with_capacity(PSRAM_BUF),
            phase: Phase::Upgrade,
            nonce: random::<32>(),
            opened_ms: now,
            last_ms: now,
            pinged_ms: now,
            closing: false,
            sent: Vec::new(),
        });
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
        if matches!(c.phase, Phase::Upgrade) {
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
                    c.phase = Phase::Hello;
                    let key = s.identity.as_ref().map(own_key).unwrap_or_default();
                    let hello = wire::hello(&key, &s.name, &c.nonce);
                    s.conns[i].send_text(&hello);
                }
            }
        } else {
            c.ws.push(&s.buf[..n]);
        }
        events(s, i);
    }
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
            WsEvent::Ping(data) => {
                let pong = wire::ws_frame(wire::OP_PONG, &data, None);
                s.conns[i].tx.extend_from_slice(&pong);
            }
            WsEvent::Pong => {}
            WsEvent::Close(code) => {
                let c = &mut s.conns[i];
                c.tx.extend_from_slice(&wire::ws_close(code.min(4999), None));
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
            c.phase = Phase::Auth { key, nonce, name };
        }
        (Phase::Auth { key, nonce, name }, "auth") => {
            let (key, their_nonce, hello_name) = (*key, *nonce, name.clone());
            let sig = wire::unhex::<64>(wire::str_of(&frame, "sig")).unwrap_or([0; 64]);
            let theirs = wire::transcript(Role::Dialler, &their_nonce, &c.nonce, &key, &own);
            if !verify(&key, &theirs, &sig) {
                return refuse(c, ErrorFrame::new("bad_signature"), "a signature that does not verify");
            }
            let mine = wire::transcript(Role::Listener, &their_nonce, &c.nonce, &key, &own);
            let our_sig = sign(s.identity.as_ref().unwrap(), &mine);
            let c = &mut s.conns[i];
            c.send_text(&wire::auth(&our_sig));
            authenticated(s, i, key, hello_name);
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

fn authenticated(s: &mut State, i: usize, key: Key, hello_name: String) {
    let (name, address) = seen_as(&s.conns[i], &hello_name);
    let seen = Seen { key: &key, name: &name, address };
    let fp = wire::fingerprint(&key);
    match s.book.classify(&seen) {
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
    // One connection per pair: an older one from the same key is stale.
    for (j, other) in s.conns.iter_mut().enumerate() {
        if j != i && !other.closing && other.key() == Some(&key) {
            other.close(wire::CLOSE_DUPLICATE, "a newer connection from the same key");
        }
    }
    s.conns[i].phase = Phase::Open { key, name };
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
    let Phase::Open { key, name } = &s.conns[i].phase else { return };
    let (key, name) = (*key, name.clone());
    let frame = match wire::check(text) {
        Ok(f) => f,
        Err(e) => {
            info(&format!("answered {name}: error {} {}", e.code, e.what.as_deref().unwrap_or("")));
            return s.conns[i].send_error(&e);
        }
    };
    let id = wire::str_of(&frame, "id").to_string();
    match wire::frame_type(&frame) {
        "msg" => {
            let m = Message {
                id: id.clone(),
                mine: false,
                ts: frame.get("ts").and_then(|v| v.as_u64()).unwrap_or(0),
                text: wire::str_of(&frame, "text").to_string(),
                state: "received".into(),
            };
            let reply_to = frame.get("reply_to").and_then(|v| v.as_str()).map(str::to_string);
            if received(s, &key, &name, m, reply_to) {
                s.conns[i].send_text(&wire::ack(&id));
            }
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
        // Not announced yet (tickets 04, 08, 09): a peer that read our
        // hello sends none of these.
        "read" | "typing" | "react" | "voice" | "call_answer" | "call_end" => {}
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

/// A message from `key`: shown if a contact's, held if a stranger's.
/// Whether to ack it: false only when it could not be kept.
fn received(s: &mut State, key: &Key, name: &str, m: Message, reply_to: Option<String>) -> bool {
    let fp = wire::fingerprint_id(key);
    let line = ChatLine {
        dir: Direction::In,
        key: wire::hex(key),
        name: name.to_string(),
        id: m.id.clone(),
        ts: m.ts,
        text: m.text.clone(),
        reply_to,
        state: m.state.clone(),
    };
    if s.book.find(key).is_some() {
        let conv = s.conv(key);
        if conv.iter().any(|x| x.id == m.id) {
            return true;
        }
        insert_by_ts(conv, m, SHOWN_MAX);
        msg_log::chat_record(false, &fp, &line);
        info(&format!("received {} from {name}", line.id));
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
    insert_by_ts(&mut req.held, m, HELD_MAX);
    msg_log::chat_record(true, &fp, &line);
    info(&format!("held {} from {name}", line.id));
    changed();
    true
}

/// Insert in `ts` order, keeping the latest `max`.
fn insert_by_ts(list: &mut Vec<Message>, m: Message, max: usize) {
    let at = list.iter().rposition(|x| x.ts <= m.ts).map_or(0, |p| p + 1);
    list.insert(at, m);
    if list.len() > max {
        list.drain(..list.len() - max);
    }
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
        let fp = wire::fingerprint_id(key);
        msg_log::chat_settle_request(&fp, true);
        let name = s.book.contacts[ci].name.clone();
        let conv = s.conv(key);
        for m in req.held {
            if !conv.iter().any(|x| x.id == m.id) {
                insert_by_ts(conv, m, SHOWN_MAX);
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
        msg_log::chat_settle_request(&wire::fingerprint_id(key), false);
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
        for (k, _) in s.convs.iter_mut().filter(|(k, _)| k == &old) {
            *k = new;
        }
        s.convs.retain(|(k, _)| k != &old);
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
        s.convs.retain(|(k, _)| k != key);
        s.outbox.retain(|(k, _)| k != key);
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
    with(|s| if s.book.find(key).is_some() { s.conv(key).clone() } else { Vec::new() }).unwrap_or_default()
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
    copy_str(&mut out.fingerprint, &wire::fingerprint(&c.key));
    copy_str(&mut out.name, &c.name);
    copy_str(&mut out.seen_name, &c.seen_name);
    out.flagged = c.flagged_key.is_some();
    copy_str(&mut out.flagged_fingerprint, &c.flagged_key.map(|k| wire::fingerprint(&k)).unwrap_or_default());
    out.connected = c.connected;
    out.accepted_by_them = c.accepted_by_them.map_or(-1, |a| a as i8);
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
    if !text.is_null() && text_len > 0 {
        let dst = unsafe { std::slice::from_raw_parts_mut(text as *mut u8, text_len) };
        copy_str(dst, &m.text);
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

// ---------------------------------------------------------------------------
// The simulator's control port, for the conformance suite
// ---------------------------------------------------------------------------

/// THISTLE_SIM_CHAT_CONTROL=PORT: a socket on 127.0.0.1:PORT speaking the
/// control protocol of docs/tailnet-chat/README.md, one JSON object a line,
/// for the user's actions and what the app shows: the conformance suite's
/// simulator target (simulator/tests/test_tailnet_chat.py). It calls the
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
            if writeln!(out, "{reply}").is_err() {
                return;
            }
        }
    }

    fn key_of(v: Option<&Value>) -> Result<Key, String> {
        v.and_then(Value::as_str).and_then(wire::unhex::<32>).ok_or_else(|| "no such key".to_string())
    }

    /// A contact by key or name.
    fn contact_key(v: Option<&Value>) -> Result<Key, String> {
        let who = v.and_then(Value::as_str).unwrap_or("");
        if let Some(k) = wire::unhex::<32>(who) {
            return Ok(k);
        }
        contacts().into_iter().find(|c| c.name == who || c.seen_name == who).map(|c| c.key)
            .ok_or_else(|| format!("no contact {who}"))
    }

    fn message_json(m: &Message) -> Value {
        json!({"id": m.id, "mine": m.mine, "ts": m.ts, "text": m.text, "state": m.state})
    }

    fn done(rc: i32) -> Result<Value, String> {
        if rc == ESP_OK { Ok(Value::Bool(true)) } else { Err(format!("0x{rc:x}")) }
    }

    fn run(cmd: &Value) -> Result<Value, String> {
        let name = cmd.get("cmd").and_then(Value::as_str).unwrap_or("");
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
                        json!({"name": c.name, "key": wire::hex(&c.key), "tailnet_name": c.seen_name,
                               "fingerprint": wire::fingerprint(&c.key),
                               "flagged_key": c.flagged_key.map(|k| wire::hex(&k)),
                               "accepted_by_them": c.accepted_by_them,
                               "declined_by_them": c.accepted_by_them == Some(false),
                               "connected": c.connected})
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
            "rename" => done(rename(&contact_key(cmd.get("contact"))?, cmd.get("name").and_then(Value::as_str).unwrap_or(""))),
            "remove" => done(remove(&contact_key(cmd.get("contact"))?)),
            "messages" => {
                let Some(k) = cmd.get("to").and_then(Value::as_str).and_then(wire::unhex::<32>) else {
                    return Ok(Value::Array(Vec::new()));
                };
                Ok(Value::Array(messages(&k).iter().map(message_json).collect()))
            }
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
        Message { id: id.into(), mine: false, ts, text: id.into(), state: "received".into() }
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
}
