// SPDX-License-Identifier: BSD-3-Clause
//! Tailnet chat's protocol, version 1, with no I/O: the handshake's
//! transcript, fingerprints, checking a JSON frame, the binary header, and
//! the WebSocket underneath (RFC 6455, the listener's side and the
//! dialler's).
//!
//! docs/tailnet-chat/protocol.md is the authority; its test vectors in
//! docs/tailnet-chat/vectors/ are this module's tests. tailnet_chat.rs
//! does the I/O, tailnet_chat_policy.rs decides who is trusted.

use serde_json::{Map, Value};
use sha2::{Digest, Sha256};

pub const VERSION: u64 = 1;
pub const PORT: u16 = 4810;
pub const PATH: &str = "/tailnet-chat/1";

/// What this device announces in its hello. `voice-message` and
/// `call:ws-opus` come with tickets 08 and 09: a cap is announced only once
/// it is done.
pub const CAPS: &[&str] = &["text", "receipts", "typing", "reply", "react"];

pub const LABEL: &[u8; 20] = b"tailnet-chat/1 auth\0";
pub const TRANSCRIPT_LEN: usize = 149;

pub const MAX_TEXT_BYTES: usize = 4096;
pub const MAX_JSON_FRAME: usize = 16384;
pub const MAX_VOICE_MS: u64 = 120_000;
pub const MAX_VOICE_BYTES: u64 = 512 * 1024;
pub const MAX_NAME_BYTES: usize = 64;
pub const MAX_CHUNK: usize = 4096;
pub const HEADER_LEN: usize = 9;

pub const KIND_VOICE: u8 = 0x01;
pub const KIND_CALL_AUDIO: u8 = 0x02;

pub const CLOSE_NORMAL: u16 = 1000;
pub const CLOSE_GOING_AWAY: u16 = 1001;
pub const CLOSE_TOO_BIG: u16 = 1009;
pub const CLOSE_PROTOCOL: u16 = 1002;
pub const CLOSE_HANDSHAKE: u16 = 4000;
pub const CLOSE_DUPLICATE: u16 = 4001;
pub const CLOSE_KEY_CHANGED: u16 = 4002;

pub type Key = [u8; 32];

// ---------------------------------------------------------------------------
// Hex, fingerprints, the transcript
// ---------------------------------------------------------------------------

pub fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(DIGITS[(b >> 4) as usize] as char);
        s.push(DIGITS[(b & 15) as usize] as char);
    }
    s
}

/// `n` bytes from lowercase hex of exactly 2n digits; None otherwise.
pub fn unhex<const N: usize>(s: &str) -> Option<[u8; N]> {
    if !is_hex(s, N * 2) {
        return None;
    }
    let mut out = [0u8; N];
    for (i, pair) in s.as_bytes().chunks(2).enumerate() {
        let d = |c: u8| if c.is_ascii_digit() { c - b'0' } else { c - b'a' + 10 };
        out[i] = (d(pair[0]) << 4) | d(pair[1]);
    }
    Some(out)
}

/// Lowercase hex of exactly `len` digits: the protocol takes no other.
pub fn is_hex(s: &str, len: usize) -> bool {
    s.len() == len && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

/// The key's fingerprint as one word: a file name, a log line.
pub fn fingerprint_id(key: &Key) -> String {
    let digest = Sha256::digest(key);
    hex(&digest[..10])
}

/// The key's fingerprint as a person compares it: five groups of four.
pub fn fingerprint(key: &Key) -> String {
    let word = fingerprint_id(key);
    let groups: Vec<&str> = (0..5).map(|i| &word[i * 4..i * 4 + 4]).collect();
    groups.join(" ")
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Dialler = 1,
    Listener = 2,
}

/// What `signer` signs in its `auth`.
pub fn transcript(signer: Role, dialler_nonce: &[u8; 32], listener_nonce: &[u8; 32],
                  dialler_key: &Key, listener_key: &Key) -> [u8; TRANSCRIPT_LEN] {
    let mut t = [0u8; TRANSCRIPT_LEN];
    t[..20].copy_from_slice(LABEL);
    t[20] = signer as u8;
    t[21..53].copy_from_slice(dialler_nonce);
    t[53..85].copy_from_slice(listener_nonce);
    t[85..117].copy_from_slice(dialler_key);
    t[117..149].copy_from_slice(listener_key);
    t
}

// ---------------------------------------------------------------------------
// JSON frames
// ---------------------------------------------------------------------------

/// An `error` frame to send back.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ErrorFrame {
    pub code: &'static str,
    pub what: Option<String>,
    pub id: Option<String>,
    pub detail: Option<String>,
}

impl ErrorFrame {
    pub fn new(code: &'static str) -> Self {
        ErrorFrame { code, what: None, id: None, detail: None }
    }
    pub fn what(mut self, what: &str) -> Self {
        self.what = Some(cut(what, 256).to_string());
        self
    }
    pub fn id(mut self, id: &str) -> Self {
        self.id = Some(id.to_string());
        self
    }
    pub fn detail(mut self, detail: &str) -> Self {
        self.detail = Some(cut(detail, 512).to_string());
        self
    }
    pub fn to_json(&self) -> String {
        let mut m = Map::new();
        m.insert("type".into(), "error".into());
        m.insert("code".into(), self.code.into());
        if let Some(w) = &self.what {
            m.insert("what".into(), w.as_str().into());
        }
        if let Some(i) = &self.id {
            m.insert("id".into(), i.as_str().into());
        }
        if let Some(d) = &self.detail {
            m.insert("detail".into(), d.as_str().into());
        }
        Value::Object(m).to_string()
    }
}

/// `s` cut to at most `max` bytes, on a character boundary.
pub fn cut(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

/// A JSON frame that checked: an object with a known `type`.
pub type Frame = Map<String, Value>;

pub fn frame_type(f: &Frame) -> &str {
    f.get("type").and_then(Value::as_str).unwrap_or("")
}

pub fn str_of<'a>(f: &'a Frame, name: &str) -> &'a str {
    f.get(name).and_then(Value::as_str).unwrap_or("")
}

enum Rule {
    Hex(usize),
    Int(u64, u64),
    Str(usize, bool),
    AnyStr,
    Bool,
    OneOf(&'static [&'static str]),
    List(&'static Rule, usize, usize),
}

const ID: Rule = Rule::Hex(32);
const KEY: Rule = Rule::Hex(64);
const TS: Rule = Rule::Int(0, 1 << 53);
const U32: Rule = Rule::Int(1, 0xFFFF_FFFF);
const CAP: Rule = Rule::Str(64, false);
const TRANSPORT: Rule = Rule::Str(32, false);

impl Rule {
    fn ok(&self, v: &Value) -> bool {
        match self {
            Rule::Hex(n) => v.as_str().is_some_and(|s| is_hex(s, *n)),
            Rule::Int(lo, hi) => v.as_u64().is_some_and(|n| (*lo..=*hi).contains(&n)),
            Rule::Str(max, empty) => v.as_str().is_some_and(|s| (*empty || !s.is_empty()) && s.len() <= *max),
            Rule::AnyStr => v.is_string(),
            Rule::Bool => v.is_boolean(),
            Rule::OneOf(values) => v.as_str().is_some_and(|s| values.contains(&s)),
            Rule::List(item, lo, hi) => v
                .as_array()
                .is_some_and(|a| (*lo..=*hi).contains(&a.len()) && a.iter().all(|x| item.ok(x))),
        }
    }
}

type Fields = &'static [(&'static str, Rule)];

/// type -> (required fields, optional fields); others are ignored.
fn schema(kind: &str) -> Option<(Fields, Fields)> {
    Some(match kind {
        "hello" => (&[("version", Rule::Int(0, 1 << 31)), ("key", KEY), ("name", Rule::Str(MAX_NAME_BYTES, true)),
                      ("caps", Rule::List(&CAP, 0, 64)), ("nonce", KEY)], &[]),
        "auth" => (&[("sig", Rule::Hex(128))], &[]),
        "msg" => (&[("id", ID), ("ts", TS), ("text", Rule::AnyStr)], &[("reply_to", ID)]),
        "ack" => (&[("id", ID)], &[]),
        "read" => (&[("ids", Rule::List(&ID, 1, 100))], &[]),
        "typing" => (&[("on", Rule::Bool)], &[]),
        "react" => (&[("id", ID), ("ts", TS), ("to", ID), ("emoji", Rule::Str(32, true))], &[]),
        "contact" => (&[("id", ID), ("state", Rule::OneOf(&["accepted", "declined"]))], &[]),
        "voice" => (&[("id", ID), ("ts", TS), ("ms", Rule::Int(1, MAX_VOICE_MS)),
                      ("bytes", Rule::Int(1, MAX_VOICE_BYTES)), ("codec", Rule::Str(32, false)),
                      ("stream", U32)], &[("reply_to", ID)]),
        "call_offer" => (&[("call", ID), ("transports", Rule::List(&TRANSPORT, 1, 8)), ("stream", U32)], &[]),
        "call_answer" => (&[("call", ID)], &[("transport", TRANSPORT), ("decline", TRANSPORT)]),
        "call_end" => (&[("call", ID), ("reason", Rule::Str(32, false))], &[]),
        "error" => (&[("code", Rule::Str(32, false))],
                    &[("what", Rule::Str(256, true)), ("id", ID), ("detail", Rule::Str(512, true))]),
        _ => return None,
    })
}

/// Parse and check one JSON text frame: the frame, or the `error` to send
/// back (protocol.md section 4).
pub fn check(text: &[u8]) -> Result<Frame, ErrorFrame> {
    if text.len() > MAX_JSON_FRAME {
        return Err(ErrorFrame::new("too_large").detail("frame over 16384 bytes"));
    }
    let Ok(text) = std::str::from_utf8(text) else {
        return Err(ErrorFrame::new("bad_request").detail("not UTF-8"));
    };
    let Ok(value) = serde_json::from_str::<Value>(text) else {
        return Err(ErrorFrame::new("bad_request").detail("not JSON"));
    };
    let Value::Object(frame) = value else {
        return Err(ErrorFrame::new("bad_request").detail("not an object with a type"));
    };
    let Some(kind) = frame.get("type").and_then(Value::as_str) else {
        return Err(ErrorFrame::new("bad_request").detail("not an object with a type"));
    };
    let Some((required, optional)) = schema(kind) else {
        return Err(ErrorFrame::new("unsupported").what(kind));
    };
    for (name, rule) in required {
        if !frame.get(*name).is_some_and(|v| rule.ok(v)) {
            return Err(ErrorFrame::new("bad_request").what(kind).detail(&format!("field {name}")));
        }
    }
    for (name, rule) in optional {
        if frame.get(*name).is_some_and(|v| !rule.ok(v)) {
            return Err(ErrorFrame::new("bad_request").what(kind).detail(&format!("field {name}")));
        }
    }
    match kind {
        "msg" => {
            let t = str_of(&frame, "text");
            if t.len() > MAX_TEXT_BYTES {
                return Err(ErrorFrame::new("too_large").what("msg").id(str_of(&frame, "id")));
            }
            if t.is_empty() {
                return Err(ErrorFrame::new("bad_request").what("msg").detail("empty text"));
            }
        }
        "voice" => {
            let codec = str_of(&frame, "codec");
            if codec != "ogg-opus" {
                return Err(ErrorFrame::new("unsupported")
                    .what(&format!("codec:{codec}"))
                    .id(str_of(&frame, "id")));
            }
        }
        "call_answer" => {
            if frame.contains_key("transport") == frame.contains_key("decline") {
                return Err(ErrorFrame::new("bad_request").what(kind).detail("transport or decline"));
            }
        }
        _ => {}
    }
    Ok(frame)
}

pub fn hello(key: &Key, name: &str, nonce: &[u8; 32]) -> String {
    let mut m = Map::new();
    m.insert("type".into(), "hello".into());
    m.insert("version".into(), VERSION.into());
    m.insert("key".into(), hex(key).into());
    m.insert("name".into(), cut(name, MAX_NAME_BYTES).into());
    m.insert("caps".into(), Value::Array(CAPS.iter().map(|c| Value::from(*c)).collect()));
    m.insert("nonce".into(), hex(nonce).into());
    Value::Object(m).to_string()
}

pub fn auth(sig: &[u8; 64]) -> String {
    format!(r#"{{"type":"auth","sig":"{}"}}"#, hex(sig))
}

pub fn ack(id: &str) -> String {
    format!(r#"{{"type":"ack","id":"{id}"}}"#)
}

/// A text message; `reply_to` the id it answers.
pub fn msg(id: &str, ts: u64, text: &str, reply_to: Option<&str>) -> String {
    let mut m = Map::new();
    m.insert("type".into(), "msg".into());
    m.insert("id".into(), id.into());
    m.insert("ts".into(), ts.into());
    m.insert("text".into(), text.into());
    if let Some(r) = reply_to {
        m.insert("reply_to".into(), r.into());
    }
    Value::Object(m).to_string()
}

/// The read receipt for `ids`, at most 100 of them.
pub fn read(ids: &[String]) -> String {
    let ids: Vec<Value> = ids.iter().take(100).map(|i| Value::from(i.as_str())).collect();
    serde_json::json!({"type": "read", "ids": ids}).to_string()
}

pub fn typing(on: bool) -> String {
    format!(r#"{{"type":"typing","on":{on}}}"#)
}

/// A reaction `emoji` to message `to`; "" removes it.
pub fn react(id: &str, ts: u64, to: &str, emoji: &str) -> String {
    serde_json::json!({"type": "react", "id": id, "ts": ts, "to": to, "emoji": emoji}).to_string()
}

pub fn contact(id: &str, accepted: bool) -> String {
    format!(r#"{{"type":"contact","id":"{id}","state":"{}"}}"#,
            if accepted { "accepted" } else { "declined" })
}

// ---------------------------------------------------------------------------
// Binary frames
// ---------------------------------------------------------------------------

#[derive(Debug, PartialEq, Eq)]
pub struct Binary<'a> {
    pub kind: u8,
    pub stream: u32,
    pub seq: u32,
    pub payload: &'a [u8],
}

pub fn unpack_binary(frame: &[u8]) -> Option<Binary<'_>> {
    if frame.len() < HEADER_LEN || frame.len() - HEADER_LEN > MAX_CHUNK {
        return None;
    }
    let be = |b: &[u8]| u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
    Some(Binary { kind: frame[0], stream: be(&frame[1..5]), seq: be(&frame[5..9]), payload: &frame[HEADER_LEN..] })
}

pub fn pack_binary(kind: u8, stream: u32, seq: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER_LEN + payload.len());
    out.push(kind);
    out.extend_from_slice(&stream.to_be_bytes());
    out.extend_from_slice(&seq.to_be_bytes());
    out.extend_from_slice(payload);
    out
}

// ---------------------------------------------------------------------------
// The WebSocket's upgrade
// ---------------------------------------------------------------------------

const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
/// The most an upgrade request may take.
pub const MAX_UPGRADE: usize = 4096;

#[derive(Debug, PartialEq, Eq)]
pub enum Upgrade {
    /// Not all of the request yet.
    Incomplete,
    /// An upgrade to the chat's path: the response to send, and how many
    /// bytes of what was read were the request.
    Accept { response: String, used: usize },
    /// Anything else: the response to send before closing.
    Refuse(&'static str),
}

pub const NOT_FOUND: &str = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
pub const BAD_REQUEST: &str = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

/// Read a client's upgrade request from what has arrived so far.
pub fn upgrade(data: &[u8]) -> Upgrade {
    let Some(end) = data.windows(4).position(|w| w == b"\r\n\r\n") else {
        return if data.len() > MAX_UPGRADE { Upgrade::Refuse(BAD_REQUEST) } else { Upgrade::Incomplete };
    };
    let head = String::from_utf8_lossy(&data[..end]);
    let mut lines = head.split("\r\n");
    let request: Vec<&str> = lines.next().unwrap_or("").split(' ').collect();
    let mut key = "";
    let mut is_upgrade = false;
    for line in lines {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        let (name, value) = (name.trim(), value.trim());
        if name.eq_ignore_ascii_case("sec-websocket-key") {
            key = value;
        } else if name.eq_ignore_ascii_case("upgrade") {
            is_upgrade = value.eq_ignore_ascii_case("websocket");
        }
    }
    if request.len() != 3 || request[0] != "GET" || request[1] != PATH || key.is_empty() || !is_upgrade {
        return Upgrade::Refuse(NOT_FOUND);
    }
    let response = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept_key(key)
    );
    Upgrade::Accept { response, used: end + 4 }
}

/// The upgrade a dialler sends, `key` its Sec-WebSocket-Key (base64 of 16
/// random bytes).
pub fn upgrade_request(host: &str, port: u16, key: &str) -> String {
    format!(
        "GET {PATH} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
         Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
}

/// A 16-byte nonce as a Sec-WebSocket-Key.
pub fn ws_key(nonce: &[u8; 16]) -> String {
    base64(nonce)
}

#[derive(Debug, PartialEq, Eq)]
pub enum Upgraded {
    /// Not all of the answer yet.
    Incomplete,
    /// 101 with the right accept key: the WebSocket follows `used` bytes in.
    Accepted { used: usize },
    /// Anything else.
    Refused,
}

/// Read the listener's answer to `upgrade_request(.., key)`.
pub fn upgraded(data: &[u8], key: &str) -> Upgraded {
    let Some(end) = data.windows(4).position(|w| w == b"\r\n\r\n") else {
        return if data.len() > MAX_UPGRADE { Upgraded::Refused } else { Upgraded::Incomplete };
    };
    let head = String::from_utf8_lossy(&data[..end]);
    let mut lines = head.split("\r\n");
    let status = lines.next().unwrap_or("");
    if status.split(' ').nth(1) != Some("101") {
        return Upgraded::Refused;
    }
    let expected = accept_key(key);
    let accepted = lines.filter_map(|l| l.split_once(':')).any(|(name, value)| {
        name.trim().eq_ignore_ascii_case("sec-websocket-accept") && value.trim() == expected
    });
    if accepted { Upgraded::Accepted { used: end + 4 } } else { Upgraded::Refused }
}

pub fn accept_key(key: &str) -> String {
    let mut input = key.as_bytes().to_vec();
    input.extend_from_slice(GUID.as_bytes());
    base64(&sha1(&input))
}

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476, 0xC3D2_E1F0];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    for block in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([block[i * 4], block[i * 4 + 1], block[i * 4 + 2], block[i * 4 + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A82_7999),
                20..=39 => (b ^ c ^ d, 0x6ED9_EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC),
                _ => (b ^ c ^ d, 0xCA62_C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }
    let mut out = [0u8; 20];
    for (i, x) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&x.to_be_bytes());
    }
    out
}

fn base64(data: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(A[((n >> (18 - 6 * i)) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// WebSocket frames
// ---------------------------------------------------------------------------

pub const OP_CONT: u8 = 0x0;
pub const OP_TEXT: u8 = 0x1;
pub const OP_BINARY: u8 = 0x2;
pub const OP_CLOSE: u8 = 0x8;
pub const OP_PING: u8 = 0x9;
pub const OP_PONG: u8 = 0xA;

/// The largest message taken: a JSON frame over 16384 bytes is answered
/// `too_large`, anything over this closes the connection (1009).
pub const MAX_MESSAGE: usize = 32 * 1024;

#[derive(Debug, PartialEq, Eq)]
pub enum WsEvent {
    Text(Vec<u8>),
    Binary(Vec<u8>),
    Ping(Vec<u8>),
    Pong,
    Close(u16),
}

/// Why a connection must close.
#[derive(Debug, PartialEq, Eq)]
pub enum WsError {
    /// A frame RFC 6455 does not allow: 1002.
    Protocol,
    /// A message over MAX_MESSAGE: 1009.
    TooBig,
}

impl WsError {
    pub fn close_code(&self) -> u16 {
        match self {
            WsError::Protocol => CLOSE_PROTOCOL,
            WsError::TooBig => CLOSE_TOO_BIG,
        }
    }
}

/// Reads frames out of what a socket delivers, joining fragments. A
/// listener's reader insists on masked frames, a dialler's on unmasked.
pub struct WsReader {
    buf: Vec<u8>,
    /// A fragmented message so far: its opcode and payload.
    partial: Option<(u8, Vec<u8>)>,
    from_client: bool,
}

impl WsReader {
    pub fn new(from_client: bool) -> Self {
        Self::with_capacity(from_client, 0)
    }

    /// With room for `capacity` bytes from the start: on the device, a
    /// buffer over 4 KB is in PSRAM.
    pub fn with_capacity(from_client: bool, capacity: usize) -> Self {
        WsReader { buf: Vec::with_capacity(capacity), partial: None, from_client }
    }

    pub fn push(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    /// Bytes waiting to be read.
    pub fn buffered(&self) -> usize {
        self.buf.len()
    }

    /// The next whole message or control frame, if one has arrived.
    pub fn next(&mut self) -> Result<Option<WsEvent>, WsError> {
        loop {
            let Some((fin, op, payload)) = self.frame()? else {
                return Ok(None);
            };
            match op {
                OP_PING => return Ok(Some(WsEvent::Ping(payload))),
                OP_PONG => return Ok(Some(WsEvent::Pong)),
                OP_CLOSE => {
                    let code = if payload.len() >= 2 { u16::from_be_bytes([payload[0], payload[1]]) } else { 1005 };
                    return Ok(Some(WsEvent::Close(code)));
                }
                OP_TEXT | OP_BINARY => {
                    if self.partial.is_some() {
                        return Err(WsError::Protocol);
                    }
                    if fin {
                        return Ok(Some(Self::message(op, payload)));
                    }
                    self.partial = Some((op, payload));
                }
                OP_CONT => {
                    let Some((first, mut so_far)) = self.partial.take() else {
                        return Err(WsError::Protocol);
                    };
                    if so_far.len() + payload.len() > MAX_MESSAGE {
                        return Err(WsError::TooBig);
                    }
                    so_far.extend_from_slice(&payload);
                    if fin {
                        return Ok(Some(Self::message(first, so_far)));
                    }
                    self.partial = Some((first, so_far));
                }
                _ => return Err(WsError::Protocol),
            }
        }
    }

    fn message(op: u8, payload: Vec<u8>) -> WsEvent {
        if op == OP_TEXT {
            WsEvent::Text(payload)
        } else {
            WsEvent::Binary(payload)
        }
    }

    /// One frame, unmasked: (fin, opcode, payload).
    fn frame(&mut self) -> Result<Option<(bool, u8, Vec<u8>)>, WsError> {
        let b = &self.buf;
        if b.len() < 2 {
            return Ok(None);
        }
        let fin = b[0] & 0x80 != 0;
        if b[0] & 0x70 != 0 {
            return Err(WsError::Protocol);
        }
        let op = b[0] & 0x0F;
        let masked = b[1] & 0x80 != 0;
        if masked != self.from_client {
            return Err(WsError::Protocol);
        }
        let (len, mut at) = match b[1] & 0x7F {
            126 => {
                if b.len() < 4 {
                    return Ok(None);
                }
                (u16::from_be_bytes([b[2], b[3]]) as u64, 4)
            }
            127 => {
                if b.len() < 10 {
                    return Ok(None);
                }
                let mut n = [0u8; 8];
                n.copy_from_slice(&b[2..10]);
                (u64::from_be_bytes(n), 10)
            }
            n => (n as u64, 2),
        };
        if op >= 0x8 && (len > 125 || !fin) {
            return Err(WsError::Protocol);
        }
        if len > MAX_MESSAGE as u64 {
            return Err(WsError::TooBig);
        }
        let len = len as usize;
        let mask = if masked {
            if b.len() < at + 4 {
                return Ok(None);
            }
            let m = [b[at], b[at + 1], b[at + 2], b[at + 3]];
            at += 4;
            Some(m)
        } else {
            None
        };
        if b.len() < at + len {
            return Ok(None);
        }
        let mut payload: Vec<u8> = self.buf[at..at + len].to_vec();
        if let Some(m) = mask {
            for (i, c) in payload.iter_mut().enumerate() {
                *c ^= m[i % 4];
            }
        }
        self.buf.drain(..at + len);
        Ok(Some((fin, op, payload)))
    }
}

/// One whole frame; masked with `mask` when a dialler sends it.
pub fn ws_frame(op: u8, payload: &[u8], mask: Option<[u8; 4]>) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + 14);
    out.push(0x80 | op);
    let bit = if mask.is_some() { 0x80 } else { 0 };
    let n = payload.len();
    if n < 126 {
        out.push(bit | n as u8);
    } else if n < 65536 {
        out.push(bit | 126);
        out.extend_from_slice(&(n as u16).to_be_bytes());
    } else {
        out.push(bit | 127);
        out.extend_from_slice(&(n as u64).to_be_bytes());
    }
    match mask {
        Some(m) => {
            out.extend_from_slice(&m);
            out.extend(payload.iter().enumerate().map(|(i, c)| c ^ m[i % 4]));
        }
        None => out.extend_from_slice(payload),
    }
    out
}

pub fn ws_close(code: u16, mask: Option<[u8; 4]>) -> Vec<u8> {
    ws_frame(OP_CLOSE, &code.to_be_bytes(), mask)
}

#[cfg(test)]
mod tests {
    use super::*;

    const HANDSHAKE: &str = include_str!("../../../docs/tailnet-chat/vectors/handshake.json");
    const MESSAGES: &str = include_str!("../../../docs/tailnet-chat/vectors/messages.json");
    const FRAMES: &str = include_str!("../../../docs/tailnet-chat/vectors/frames.json");

    fn v(text: &str) -> Value {
        serde_json::from_str(text).unwrap()
    }

    fn bytes(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    fn key(s: &Value) -> Key {
        unhex::<32>(s.as_str().unwrap()).unwrap()
    }

    #[test]
    fn a_dialled_upgrade_is_answered_and_its_answer_checked() {
        let key = "dGhlIHNhbXBsZSBub25jZQ==";
        let request = upgrade_request("100.101.1.2", 4810, key);
        assert!(request.starts_with("GET /tailnet-chat/1 HTTP/1.1\r\nHost: 100.101.1.2:4810\r\n"), "{request}");
        let Upgrade::Accept { response, .. } = upgrade(request.as_bytes()) else { panic!("refused: {request}") };
        let mut answer = response.into_bytes();
        assert_eq!(upgraded(&answer[..10], key), Upgraded::Incomplete);
        answer.extend_from_slice(&[0x81, 0x00]);
        assert_eq!(upgraded(&answer, key), Upgraded::Accepted { used: answer.len() - 2 });
        assert_eq!(upgraded(NOT_FOUND.as_bytes(), key), Upgraded::Refused);
        let wrong = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: xyz\r\n\r\n";
        assert_eq!(upgraded(wrong.as_bytes(), key), Upgraded::Refused, "the wrong accept key");
    }

    #[test]
    fn the_frames_this_device_sends_pass_the_schema() {
        let id = "ab".repeat(16);
        let to = "cd".repeat(16);
        for (frame, kind) in [
            (msg(&id, 1_793_541_600_000, "مرحبا \"quoted\"\n", None), "msg"),
            (msg(&id, 1, "a reply", Some(&to)), "msg"),
            (read(&[id.clone(), to.clone()]), "read"),
            (typing(true), "typing"),
            (typing(false), "typing"),
            (react(&id, 5, &to, "👍"), "react"),
            (react(&id, 5, &to, ""), "react"),
        ] {
            let f = check(frame.as_bytes()).unwrap_or_else(|e| panic!("{frame}: {}", e.to_json()));
            assert_eq!(frame_type(&f), kind);
        }
        let m = check(msg(&id, 1, "a reply", Some(&to)).as_bytes()).unwrap();
        assert_eq!(str_of(&m, "reply_to"), to);
        assert_eq!(str_of(&check(msg(&id, 1, "x\"y", None).as_bytes()).unwrap(), "text"), "x\"y");
    }

    #[test]
    fn the_hello_announces_what_this_device_does() {
        let h = check(hello(&[1; 32], "t", &[2; 32]).as_bytes()).unwrap();
        let caps: Vec<&str> = h["caps"].as_array().unwrap().iter().map(|c| c.as_str().unwrap()).collect();
        assert_eq!(caps, ["text", "receipts", "typing", "reply", "react"]);
    }

    #[test]
    fn the_transcripts_are_the_vectors() {
        let h = v(HANDSHAKE);
        let (d, l) = (&h["dialler"], &h["listener"]);
        let nd = unhex::<32>(d["nonce"].as_str().unwrap()).unwrap();
        let nl = unhex::<32>(l["nonce"].as_str().unwrap()).unwrap();
        let (kd, kl) = (key(&d["key"]), key(&l["key"]));
        assert_eq!(hex(&transcript(Role::Dialler, &nd, &nl, &kd, &kl)), h["transcripts"]["dialler"]);
        assert_eq!(hex(&transcript(Role::Listener, &nd, &nl, &kd, &kl)), h["transcripts"]["listener"]);
    }

    #[test]
    fn the_signatures_are_the_vectors_and_the_refused_ones_do_not_verify() {
        use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
        let h = v(HANDSHAKE);
        let (d, l) = (&h["dialler"], &h["listener"]);
        for (side, role) in [(d, "dialler"), (l, "listener")] {
            let seed = unhex::<32>(side["seed"].as_str().unwrap()).unwrap();
            let sk = SigningKey::from_bytes(&seed);
            assert_eq!(hex(sk.verifying_key().as_bytes()), side["key"]);
            let t = bytes(h["transcripts"][role].as_str().unwrap());
            let sig = sk.sign(&t).to_bytes();
            assert_eq!(auth(&sig), side["auth"].as_str().unwrap());
            assert_eq!(fingerprint(&key(&side["key"])), side["fingerprint"]);
        }
        let kd = VerifyingKey::from_bytes(&key(&d["key"])).unwrap();
        let t = bytes(h["transcripts"]["dialler"].as_str().unwrap());
        for refused in h["refused"].as_array().unwrap() {
            let f = check(refused["auth"].as_str().unwrap().as_bytes()).unwrap();
            let sig = unhex::<64>(str_of(&f, "sig")).unwrap();
            assert!(kd.verify(&t, &Signature::from_bytes(&sig)).is_err(), "{}", refused["why"]);
        }
    }

    #[test]
    fn our_hello_checks_and_carries_the_key_and_nonce() {
        let h = v(HANDSHAKE);
        let k = key(&h["listener"]["key"]);
        let nonce = [7u8; 32];
        let f = check(hello(&k, "T-Deck Max", &nonce).as_bytes()).unwrap();
        assert_eq!(str_of(&f, "key"), hex(&k));
        assert_eq!(str_of(&f, "nonce"), hex(&nonce));
        assert_eq!(f["version"], 1);
        assert!(f["caps"].as_array().unwrap().iter().any(|c| c == "text"));
        assert!(check(ack(&"a".repeat(32)).as_bytes()).is_ok());
        assert!(check(contact(&"b".repeat(32), true).as_bytes()).is_ok());
    }

    #[test]
    fn every_valid_frame_of_the_vectors_checks() {
        for example in v(MESSAGES)["valid"].as_array().unwrap() {
            let text = example["text"].as_str().unwrap();
            let f = check(text.as_bytes()).unwrap_or_else(|e| panic!("{}: {e:?}", example["name"]));
            assert_eq!(Value::Object(f), v(text));
        }
    }

    #[test]
    fn every_invalid_frame_of_the_vectors_draws_its_error() {
        for example in v(MESSAGES)["invalid"].as_array().unwrap() {
            let err = check(example["text"].as_str().unwrap().as_bytes())
                .expect_err(example["name"].as_str().unwrap());
            let want = &example["error"];
            assert_eq!(err.code, want["code"], "{}", example["name"]);
            for name in ["what", "id"] {
                if let Some(w) = want.get(name) {
                    let got = if name == "what" { &err.what } else { &err.id };
                    assert_eq!(got.as_deref(), w.as_str(), "{} {name}", example["name"]);
                }
            }
            let sent = check(err.to_json().as_bytes()).unwrap();
            assert_eq!(frame_type(&sent), "error");
        }
    }

    #[test]
    fn members_in_any_order_and_unknown_ones_are_taken() {
        let f = check(br#"{ "id" : "aace078b1c550597f0080d2e140ab897", "later": [1], "type":"ack" }"#).unwrap();
        assert_eq!(str_of(&f, "id"), "aace078b1c550597f0080d2e140ab897");
        let big = format!(r#"{{"type":"msg","id":"{}","ts":1,"text":"{}"}}"#, "a".repeat(32), "x".repeat(17000));
        assert_eq!(check(big.as_bytes()).unwrap_err().code, "too_large");
        assert_eq!(check(&[0xff, 0xfe]).unwrap_err().code, "bad_request");
        assert_eq!(check(br#"{"type":"msg","id":"aace078b1c550597f0080d2e140ab897","ts":1.5,"text":"x"}"#)
            .unwrap_err().code, "bad_request");
    }

    #[test]
    fn binary_frames_are_the_vectors() {
        let f = v(FRAMES);
        for example in f["valid"].as_array().unwrap() {
            let frame = bytes(example["frame"].as_str().unwrap());
            let b = unpack_binary(&frame).unwrap();
            assert_eq!(b.kind as u64, example["kind"].as_u64().unwrap());
            assert_eq!(b.stream as u64, example["stream"].as_u64().unwrap());
            assert_eq!(b.seq as u64, example["seq"].as_u64().unwrap());
            assert_eq!(hex(b.payload), example["payload"]);
            assert_eq!(pack_binary(b.kind, b.stream, b.seq, b.payload), frame);
        }
        for example in f["invalid"].as_array().unwrap() {
            let frame = bytes(example["frame"].as_str().unwrap());
            assert!(unpack_binary(&frame).is_none(), "{}", example["why"]);
        }
    }

    #[test]
    fn hex_is_lowercase_and_exact() {
        assert_eq!(unhex::<2>("0aff"), Some([0x0a, 0xff]));
        assert_eq!(unhex::<2>("0AFF"), None);
        assert_eq!(unhex::<2>("0af"), None);
        assert_eq!(cut("éé", 3), "é");
    }

    #[test]
    fn the_upgrade_answers_rfc_6455s_example() {
        assert_eq!(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        let req = b"GET /tailnet-chat/1 HTTP/1.1\r\nHost: x\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\nrest";
        assert_eq!(upgrade(&req[..20]), Upgrade::Incomplete);
        match upgrade(req) {
            Upgrade::Accept { response, used } => {
                assert!(response.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
                assert_eq!(&req[used..], b"rest");
            }
            other => panic!("{other:?}"),
        }
        let other = b"GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: a\r\n\r\n";
        assert_eq!(upgrade(other), Upgrade::Refuse(NOT_FOUND));
        assert_eq!(upgrade(&[b'a'; 5000]), Upgrade::Refuse(BAD_REQUEST));
    }

    #[test]
    fn frames_are_read_masked_joined_and_answered() {
        let mask = Some([1, 2, 3, 4]);
        let mut r = WsReader::new(true);
        let mut wire = ws_frame(OP_TEXT, b"hello", mask);
        // A fragmented binary message with a ping between its pieces
        let mut first = ws_frame(OP_BINARY, b"ab", mask);
        first[0] &= 0x7F;
        wire.extend(first);
        wire.extend(ws_frame(OP_PING, b"p", mask));
        wire.extend(ws_frame(OP_CONT, &[b'c'; 300], mask));
        wire.extend(ws_close(4002, mask));
        // Delivered a byte at a time
        let mut got = Vec::new();
        for b in &wire {
            r.push(std::slice::from_ref(b));
            while let Some(e) = r.next().unwrap() {
                got.push(e);
            }
        }
        let mut joined = b"ab".to_vec();
        joined.extend([b'c'; 300]);
        assert_eq!(got, vec![WsEvent::Text(b"hello".to_vec()), WsEvent::Ping(b"p".to_vec()),
                             WsEvent::Binary(joined), WsEvent::Close(4002)]);
    }

    #[test]
    fn a_listener_refuses_unmasked_and_oversized_frames() {
        let mut r = WsReader::new(true);
        r.push(&ws_frame(OP_TEXT, b"x", None));
        assert_eq!(r.next(), Err(WsError::Protocol));
        let mut r = WsReader::new(true);
        r.push(&ws_frame(OP_BINARY, &vec![0; MAX_MESSAGE + 1], Some([0; 4])));
        assert_eq!(r.next(), Err(WsError::TooBig));
        let mut r = WsReader::new(false);
        r.push(&ws_frame(OP_TEXT, &vec![b'y'; 70000], None)[..20]);
        assert_eq!(r.next(), Err(WsError::TooBig));
    }
}
