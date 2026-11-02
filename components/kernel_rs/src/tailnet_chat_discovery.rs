// SPDX-License-Identifier: BSD-3-Clause
//! Looking for chat peers on the tailnet, when the app asks (spec,
//! "Discovery" and "Further Notes"; protocol.md section 9): each online
//! tailnet peer the kernel keeps (tailnet_manager.rs), less this device,
//! is probed on the chat port with a hello. A probe is a dial, the
//! upgrade, a hello each way and a close: no `auth`, so nothing is held on
//! the other side. Never in the background.
//!
//! The probes run one after another on a task made for the look
//! (`chat_look`, its stack in internal RAM) and gone after, holding the
//! device awake meanwhile, at most PROBE_TIMEOUT_MS for each connect and
//! each hello. What answered is kept until the next look; which of it is
//! shown, and which peers are probed, is tailnet_chat_policy.rs'
//! (`to_probe`, `to_show`). Add adds one as Add contact does, by the
//! address it answered on, so its key is pinned at the first handshake.

use super::*;
use crate::tailnet_chat_policy::{Found, ProbePeer, PROBE_TIMEOUT_MS};

/// The look's task's stack: a hello parsed and one written.
const LOOK_STACK: usize = 8 * 1024;

struct Look {
    looking: bool,
    /// A look has ended since the boot.
    looked: bool,
    probed: usize,
    peers: usize,
    found: Vec<Found>,
}

static LOOK: Mutex<Look> = Mutex::new(Look { looking: false, looked: false, probed: 0, peers: 0, found: Vec::new() });

fn look() -> MutexGuard<'static, Look> {
    LOOK.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Where a look stands: looking, a look ended, peers probed so far, peers
/// to probe.
pub(crate) fn progress() -> (bool, bool, usize, usize) {
    let l = look();
    (l.looking, l.looked, l.probed, l.peers)
}

/// Start a look, unless one is under way. ESP_ERR_INVALID_STATE while
/// chat is not listening.
pub(crate) fn discover() -> i32 {
    let Some((own, name, port, own_ip)) = with(|s| {
        let at = s.listening_on?;
        let id = s.identity.as_ref()?;
        let own_ip = match at.ip() {
            IpAddr::V4(v4) => u32::from(v4),
            IpAddr::V6(_) => 0,
        };
        Some((own_key(id), s.name.clone(), s.settings.port, own_ip))
    })
    .flatten() else {
        return ESP_ERR_INVALID_STATE;
    };
    let peers: Vec<ProbePeer> = crate::tailnet_manager::peers()
        .into_iter()
        .map(|p| ProbePeer { ip: p.ip, name: p.name, online: p.online })
        .collect();
    let chosen: Vec<ProbePeer> =
        policy::to_probe(&peers, own_ip, SIMULATOR).into_iter().map(|i| peers[i].clone()).collect();
    {
        let mut l = look();
        if l.looking {
            return ESP_OK;
        }
        *l = Look { looking: true, looked: l.looked, probed: 0, peers: chosen.len(), found: Vec::new() };
    }
    info(&format!("looking for chat peers: {} of {} tailnet peers to ask", chosen.len(), peers.len()));
    changed();
    let job = move || {
        let _awake = HoldGuard::new(b"chat discovery\0");
        for p in &chosen {
            let addr = SocketAddr::new(IpAddr::V4(Ipv4Addr::from(p.ip)), port);
            let heard = probe(addr, &own, &name);
            let mut l = look();
            l.probed += 1;
            match heard {
                Some((key, hello_name)) => {
                    info(&format!("{} at {addr} is a chat peer: {hello_name} {}", p.name, wire::fingerprint(&key)));
                    l.found.push(Found { key, name: hello_name, tailnet_name: p.name.clone(), ip: addr.ip(), port });
                }
                None => info(&format!("{} at {addr}: no chat peer answered", p.name)),
            }
            drop(l);
            changed();
        }
        let mut l = look();
        info(&format!("found {} chat peers among {} tailnet peers", l.found.len(), l.peers));
        l.looking = false;
        l.looked = true;
        drop(l);
        changed();
    };
    if !crate::kernel_task::spawn(c"chat_look", LOOK_STACK, crate::kernel_task::PRIORITY_DEFAULT, job) {
        let mut l = look();
        l.looking = false;
        warn("could not look for chat peers: no task");
        return ESP_ERR_NO_MEM;
    }
    ESP_OK
}

/// Whether a chat peer answers at `addr`, and the key and name its hello
/// announces (not proven).
fn probe(addr: SocketAddr, own: &Key, name: &str) -> Option<(Key, String)> {
    let wait = Duration::from_millis(PROBE_TIMEOUT_MS);
    let mut sock = TcpStream::connect_timeout(&addr, wait).ok()?;
    sock.set_read_timeout(Some(wait)).ok()?;
    sock.set_write_timeout(Some(wait)).ok()?;
    let _ = sock.set_nodelay(true);
    let ws_key = wire::ws_key(&random::<16>());
    sock.write_all(wire::upgrade_request(&addr.ip().to_string(), addr.port(), &ws_key).as_bytes()).ok()?;
    let deadline = Instant::now() + wait;
    let mut head: Vec<u8> = Vec::new();
    let mut reader: Option<WsReader> = None;
    let mut buf = [0u8; 512];
    let heard = loop {
        if Instant::now() >= deadline {
            break None;
        }
        let n = match sock.read(&mut buf) {
            Ok(0) | Err(_) => break None,
            Ok(n) => n,
        };
        let r = match reader.as_mut() {
            Some(r) => {
                r.push(&buf[..n]);
                r
            }
            None => {
                head.extend_from_slice(&buf[..n]);
                match wire::upgraded(&head, &ws_key) {
                    Upgraded::Incomplete => continue,
                    Upgraded::Refused => break None,
                    Upgraded::Accepted { used } => {
                        let hello = wire::hello(own, name, &random::<32>());
                        if sock.write_all(&wire::ws_frame(wire::OP_TEXT, hello.as_bytes(), Some(random::<4>()))).is_err() {
                            break None;
                        }
                        let mut r = WsReader::new(false);
                        r.push(&head[used..]);
                        reader.insert(r)
                    }
                }
            }
        };
        match r.next() {
            Ok(Some(WsEvent::Text(text))) => break policy::heard(&text),
            Ok(Some(WsEvent::Ping(_) | WsEvent::Pong)) | Ok(None) => continue,
            _ => break None,
        }
    };
    if reader.is_some() {
        let _ = sock.write_all(&wire::ws_close(wire::CLOSE_NORMAL, Some(random::<4>())));
    }
    heard
}

/// What the last look found that the app shows: not this device, not a
/// contact.
pub(crate) fn found() -> Vec<Found> {
    let all = look().found.clone();
    with(|s| {
        let own = s.identity.as_ref().map(own_key).unwrap_or_default();
        policy::to_show(&all, &s.book, &own).into_iter().map(|i| all[i].clone()).collect()
    })
    .unwrap_or_default()
}

/// Add the chat peer found with `key` as a contact, by the address it
/// answered on and its hello's name: as Add contact does, so its key is
/// pinned at the first handshake. The key it goes by until then.
pub(crate) fn add_found(key: &Key) -> Result<Key, i32> {
    let f = found().into_iter().find(|f| &f.key == key).ok_or(ESP_ERR_NOT_FOUND)?;
    add_contact(&f.ip.to_string(), f.port, &f.name)
}

// ---------------------------------------------------------------------------
// The C API (thistle/tailnet_chat.h)
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct CDiscovery {
    looking: bool,
    looked: bool,
    probed: u16,
    peers: u16,
}

#[repr(C)]
pub struct CFound {
    key: [u8; 65],
    fingerprint: [u8; 25],
    name: [u8; 65],
    tailnet_name: [u8; 65],
    address: [u8; 46],
    port: u16,
}

#[no_mangle]
pub extern "C" fn tailnet_chat_discover() -> i32 {
    discover()
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_chat_discovery_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_discovery(out: *mut CDiscovery) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let (looking, looked, probed, peers) = progress();
    *out = CDiscovery { looking, looked, probed: probed as u16, peers: peers as u16 };
    ESP_OK
}

#[no_mangle]
pub extern "C" fn tailnet_chat_found_count() -> i32 {
    found().len() as i32
}

/// # Safety
/// `out` must be null or point to a writable `tailnet_chat_found_t`.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_get_found(index: i32, out: *mut CFound) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else { return ESP_ERR_INVALID_ARG };
    let Some(f) = usize::try_from(index).ok().and_then(|i| found().into_iter().nth(i)) else {
        return ESP_ERR_INVALID_ARG;
    };
    copy_str(&mut out.key, &wire::hex(&f.key));
    copy_str(&mut out.fingerprint, &wire::fingerprint(&f.key));
    copy_str(&mut out.name, &f.name);
    copy_str(&mut out.tailnet_name, f.tailnet_name.split('.').next().unwrap_or(""));
    copy_str(&mut out.address, &f.ip.to_string());
    out.port = f.port;
    ESP_OK
}

/// # Safety
/// `key` must be null or a NUL-terminated string, `key_out` null or 65
/// writable bytes.
#[no_mangle]
pub unsafe extern "C" fn tailnet_chat_add_found(key: *const c_char, key_out: *mut c_char) -> i32 {
    let Some(key) = (unsafe { key_arg(key) }) else { return ESP_ERR_INVALID_ARG };
    match add_found(&key) {
        Ok(k) => {
            if !key_out.is_null() {
                copy_str(unsafe { std::slice::from_raw_parts_mut(key_out as *mut u8, 65) }, &wire::hex(&k));
            }
            ESP_OK
        }
        Err(e) => e,
    }
}
