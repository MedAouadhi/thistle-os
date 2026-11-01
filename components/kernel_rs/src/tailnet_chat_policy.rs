// SPDX-License-Identifier: BSD-3-Clause
//! Tailnet chat's decisions, with no I/O: who is admitted to the handshake,
//! who a proven key is (a contact, a contact's impostor, a stranger), what
//! accepting, renaming and removing do to the contacts, and the module's
//! file, in which they are kept.
//!
//! protocol.md section 7 has the rules; tailnet_chat.rs runs them.

use std::net::IpAddr;

use serde_json::{json, Map, Value};

use crate::tailnet_chat_wire::{self as wire, Key};

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------

/// Whether a connection from `ip` may reach the handshake: the tailnet's
/// ranges, 100.64.0.0/10 and fd7a:115c:a1e0::/48 (an IPv4-mapped address
/// counts as its IPv4 address), and loopback where `loopback` allows it
/// (the simulator).
pub fn admitted(ip: IpAddr, loopback: bool) -> bool {
    let ip = match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map(IpAddr::V4).unwrap_or(IpAddr::V6(v6)),
        v4 => v4,
    };
    if loopback && ip.is_loopback() {
        return true;
    }
    on_tailnet(ip)
}

/// Whether `ip` is a tailnet address.
pub fn on_tailnet(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => {
            let o = v4.octets();
            o[0] == 100 && (o[1] & 0xC0) == 64
        }
        IpAddr::V6(v6) => {
            let s = v6.segments();
            s[0] == 0xfd7a && s[1] == 0x115c && s[2] == 0xa1e0
        }
    }
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------

/// The longest name a contact is given.
pub const NAME_MAX: usize = 64;
/// The most contacts kept.
pub const CONTACTS_MAX: usize = 32;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Contact {
    /// What the user calls it; the name it came under until renamed.
    pub name: String,
    /// The pinned identity key: who the contact is.
    pub key: Key,
    /// The name it was last seen under: its tailnet hostname, or else its
    /// hello's name. Another key under it is an impostor.
    pub seen_name: String,
    /// The tailnet address it was last seen at, if any; another key from it
    /// is an impostor.
    pub address: Option<IpAddr>,
    /// Another key seen under its name or address, refused, until the user
    /// accepts it in this one's place.
    pub flagged_key: Option<Key>,
    /// What it answered this device's contact request, if anything.
    pub accepted_by_them: Option<bool>,
}

/// Who a chat peer is, as its connection showed it.
#[derive(Clone, Debug)]
pub struct Seen<'a> {
    pub key: &'a Key,
    /// Its tailnet hostname where the platform knows it, else its hello's.
    pub name: &'a str,
    /// The tailnet address it came from or was dialled at. Never loopback,
    /// where every test peer shares one.
    pub address: Option<IpAddr>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Trust {
    /// A contact, by its index: its frames are taken.
    Contact(usize),
    /// Another key under the contact's name or address: refused with
    /// `key_changed` and flagged.
    KeyChanged(usize),
    /// Anything else: its messages are held as a contact request.
    Stranger,
}

fn same_name(a: &str, b: &str) -> bool {
    !a.trim().is_empty() && a.trim().eq_ignore_ascii_case(b.trim())
}

/// A name as a contact keeps it: trimmed, one line, at most NAME_MAX bytes.
pub fn clean_name(name: &str) -> Option<String> {
    let one_line: String = name.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    let trimmed = wire::cut(one_line.trim(), NAME_MAX).trim();
    (!trimmed.is_empty()).then(|| trimmed.to_string())
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Book {
    pub contacts: Vec<Contact>,
}

impl Book {
    pub fn find(&self, key: &Key) -> Option<usize> {
        self.contacts.iter().position(|c| &c.key == key)
    }

    /// Who `seen` is (protocol.md section 7).
    pub fn classify(&self, seen: &Seen) -> Trust {
        if let Some(i) = self.find(seen.key) {
            return Trust::Contact(i);
        }
        let impostor = self.contacts.iter().position(|c| {
            let by_address = seen.address.is_some() && c.address == seen.address;
            by_address || same_name(&c.seen_name, seen.name)
        });
        match impostor {
            Some(i) => Trust::KeyChanged(i),
            None => Trust::Stranger,
        }
    }

    /// A contact seen with its own key: its name and address may have
    /// changed, and it is still the same contact.
    pub fn seen(&mut self, i: usize, seen: &Seen) {
        let c = &mut self.contacts[i];
        if let Some(name) = clean_name(seen.name) {
            c.seen_name = name;
        }
        if seen.address.is_some() {
            c.address = seen.address;
        }
    }

    /// Flag another key under contact `i`'s name or address.
    pub fn flag(&mut self, i: usize, key: &Key) {
        self.contacts[i].flagged_key = Some(*key);
    }

    /// Accept a contact request: pin the stranger's key. Its index, or None
    /// when the book is full.
    pub fn accept(&mut self, seen: &Seen) -> Option<usize> {
        if let Some(i) = self.find(seen.key) {
            return Some(i);
        }
        if self.contacts.len() >= CONTACTS_MAX {
            return None;
        }
        let name = clean_name(seen.name).unwrap_or_else(|| wire::fingerprint(seen.key));
        self.contacts.push(Contact {
            name: name.clone(),
            key: *seen.key,
            seen_name: name,
            address: seen.address,
            flagged_key: None,
            accepted_by_them: None,
        });
        Some(self.contacts.len() - 1)
    }

    /// Put contact `i`'s flagged key in place of its pinned one. The old
    /// key, or None if nothing was flagged.
    pub fn accept_new_key(&mut self, i: usize) -> Option<Key> {
        let c = self.contacts.get_mut(i)?;
        let new = c.flagged_key.take()?;
        // Another contact pinned to the same key would be two of one.
        let old = std::mem::replace(&mut c.key, new);
        c.accepted_by_them = None;
        let dup = self.contacts.iter().enumerate().position(|(j, c)| j != i && c.key == new);
        if let Some(j) = dup {
            self.contacts.remove(j);
        }
        Some(old)
    }

    pub fn rename(&mut self, i: usize, name: &str) -> bool {
        match (self.contacts.get_mut(i), clean_name(name)) {
            (Some(c), Some(name)) => {
                c.name = name;
                true
            }
            _ => false,
        }
    }

    /// Unpin contact `i`: its next message is a contact request again.
    pub fn remove(&mut self, i: usize) -> Option<Contact> {
        (i < self.contacts.len()).then(|| self.contacts.remove(i))
    }
}

// ---------------------------------------------------------------------------
// The module's file
// ---------------------------------------------------------------------------

/// Frames waiting for a contact's ack: (key, [(id, frame)]).
pub type Outbox = Vec<(Key, Vec<(String, String)>)>;

fn key_json(k: &Key) -> Value {
    Value::from(wire::hex(k))
}

/// The contacts and the outbox as the module's file keeps them. Nothing
/// secret: keys here are public ones.
pub fn to_file(book: &Book, outbox: &Outbox) -> String {
    let contacts: Vec<Value> = book
        .contacts
        .iter()
        .map(|c| {
            let mut m = Map::new();
            m.insert("name".into(), c.name.as_str().into());
            m.insert("key".into(), key_json(&c.key));
            m.insert("seen_name".into(), c.seen_name.as_str().into());
            if let Some(a) = c.address {
                m.insert("address".into(), a.to_string().into());
            }
            if let Some(k) = &c.flagged_key {
                m.insert("flagged_key".into(), key_json(k));
            }
            if let Some(b) = c.accepted_by_them {
                m.insert("accepted_by_them".into(), b.into());
            }
            Value::Object(m)
        })
        .collect();
    let outbox: Vec<Value> = outbox
        .iter()
        .filter(|(_, frames)| !frames.is_empty())
        .map(|(k, frames)| {
            json!({"key": wire::hex(k),
                   "frames": frames.iter().map(|(id, f)| json!({"id": id, "frame": f})).collect::<Vec<_>>()})
        })
        .collect();
    let mut text = json!({"version": 1, "contacts": contacts, "outbox": outbox}).to_string();
    text.push('\n');
    text
}

/// Read the module's file back; whatever of it is unreadable is left out.
pub fn from_file(text: &str) -> (Book, Outbox) {
    let Ok(v) = serde_json::from_str::<Value>(text) else {
        return (Book::default(), Vec::new());
    };
    let key = |v: Option<&Value>| v.and_then(Value::as_str).and_then(wire::unhex::<32>);
    let contacts = v
        .get("contacts")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|c| {
                    let k = key(c.get("key"))?;
                    let name = clean_name(c.get("name")?.as_str()?)?;
                    Some(Contact {
                        seen_name: c.get("seen_name").and_then(Value::as_str).unwrap_or(&name).to_string(),
                        name,
                        key: k,
                        address: c.get("address").and_then(Value::as_str).and_then(|a| a.parse().ok()),
                        flagged_key: key(c.get("flagged_key")),
                        accepted_by_them: c.get("accepted_by_them").and_then(Value::as_bool),
                    })
                })
                .take(CONTACTS_MAX)
                .collect()
        })
        .unwrap_or_default();
    let outbox = v
        .get("outbox")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|e| {
                    let k = key(e.get("key"))?;
                    let frames = e
                        .get("frames")?
                        .as_array()?
                        .iter()
                        .filter_map(|f| Some((f.get("id")?.as_str()?.to_string(), f.get("frame")?.as_str()?.to_string())))
                        .collect();
                    Some((k, frames))
                })
                .collect()
        })
        .unwrap_or_default();
    (Book { contacts }, outbox)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{Ipv4Addr, Ipv6Addr};

    fn ip(s: &str) -> IpAddr {
        s.parse().unwrap()
    }

    const ALICE: Key = [1; 32];
    const MALLORY: Key = [2; 32];
    const BOB: Key = [3; 32];

    fn seen<'a>(key: &'a Key, name: &'a str, address: Option<IpAddr>) -> Seen<'a> {
        Seen { key, name, address }
    }

    fn book_with_alice() -> Book {
        let mut b = Book::default();
        b.accept(&seen(&ALICE, "laptop", Some(ip("100.101.1.2")))).unwrap();
        b
    }

    #[test]
    fn only_the_tailnet_is_admitted_and_loopback_where_allowed() {
        for a in ["100.64.0.1", "100.127.255.255", "100.101.102.103", "fd7a:115c:a1e0::1", "::ffff:100.64.0.9"] {
            assert!(admitted(ip(a), false), "{a}");
        }
        for a in ["100.63.255.255", "100.128.0.0", "192.168.1.5", "10.0.0.1", "fd7a:115c:a1e1::1", "::ffff:192.168.1.5"] {
            assert!(!admitted(ip(a), false), "{a}");
            assert!(!admitted(ip(a), true), "{a}");
        }
        assert!(!admitted(IpAddr::V4(Ipv4Addr::LOCALHOST), false));
        assert!(admitted(IpAddr::V4(Ipv4Addr::LOCALHOST), true));
        assert!(admitted(IpAddr::V6(Ipv6Addr::LOCALHOST), true));
    }

    #[test]
    fn a_key_never_seen_is_a_stranger() {
        assert_eq!(Book::default().classify(&seen(&ALICE, "laptop", None)), Trust::Stranger);
        let b = book_with_alice();
        assert_eq!(b.classify(&seen(&BOB, "phone", Some(ip("100.101.1.3")))), Trust::Stranger);
    }

    #[test]
    fn an_accepted_key_is_a_contact_under_any_name_or_address() {
        let mut b = book_with_alice();
        let renamed = seen(&ALICE, "laptop-2", Some(ip("100.101.9.9")));
        assert_eq!(b.classify(&renamed), Trust::Contact(0));
        b.seen(0, &renamed);
        assert_eq!(b.contacts[0].seen_name, "laptop-2");
        assert_eq!(b.contacts[0].address, Some(ip("100.101.9.9")));
        // and the old name is now free for another key
        assert_eq!(b.classify(&seen(&BOB, "laptop", None)), Trust::Stranger);
    }

    #[test]
    fn another_key_under_a_contacts_name_or_address_is_a_mismatch() {
        let b = book_with_alice();
        assert_eq!(b.classify(&seen(&MALLORY, "laptop", None)), Trust::KeyChanged(0));
        assert_eq!(b.classify(&seen(&MALLORY, "LAPTOP ", None)), Trust::KeyChanged(0));
        assert_eq!(b.classify(&seen(&MALLORY, "other", Some(ip("100.101.1.2")))), Trust::KeyChanged(0));
        assert_eq!(b.classify(&seen(&MALLORY, "", None)), Trust::Stranger);
    }

    #[test]
    fn a_new_key_accepted_takes_the_old_ones_place() {
        let mut b = book_with_alice();
        b.rename(0, "Alice");
        b.flag(0, &MALLORY);
        assert_eq!(b.contacts[0].flagged_key, Some(MALLORY));
        assert_eq!(b.accept_new_key(0), Some(ALICE));
        assert_eq!(b.classify(&seen(&MALLORY, "laptop", None)), Trust::Contact(0));
        assert_eq!(b.contacts[0].name, "Alice");
        assert_eq!(b.contacts[0].flagged_key, None);
        // the old key under that name is now the mismatch
        assert_eq!(b.classify(&seen(&ALICE, "laptop", None)), Trust::KeyChanged(0));
        assert_eq!(b.accept_new_key(0), None, "nothing flagged");
    }

    #[test]
    fn a_removed_contact_asks_again() {
        let mut b = book_with_alice();
        assert_eq!(b.remove(0).map(|c| c.key), Some(ALICE));
        assert_eq!(b.classify(&seen(&ALICE, "laptop", None)), Trust::Stranger);
        assert_eq!(b.classify(&seen(&MALLORY, "laptop", None)), Trust::Stranger);
        assert!(b.remove(0).is_none());
    }

    #[test]
    fn accepting_twice_is_one_contact_and_names_are_kept_tidy() {
        let mut b = book_with_alice();
        assert_eq!(b.accept(&seen(&ALICE, "x", None)), Some(0));
        assert_eq!(b.contacts.len(), 1);
        let i = b.accept(&seen(&BOB, "  ", None)).unwrap();
        assert_eq!(b.contacts[i].name, wire::fingerprint(&BOB), "a nameless one is called by its fingerprint");
        assert!(!b.rename(i, " \n "));
        assert!(b.rename(i, "Bob\nSmith"));
        assert_eq!(b.contacts[i].name, "Bob Smith");
        assert!(b.rename(i, &"é".repeat(40)));
        assert_eq!(b.contacts[i].name.len(), 64);
    }

    #[test]
    fn the_book_is_full_at_its_limit() {
        let mut b = Book::default();
        for n in 0..CONTACTS_MAX {
            let k = [n as u8 + 10; 32];
            assert!(b.accept(&seen(&k, &format!("p{n}"), None)).is_some());
        }
        assert_eq!(b.accept(&seen(&BOB, "late", None)), None);
    }

    #[test]
    fn the_file_keeps_contacts_flags_and_the_outbox() {
        let mut b = book_with_alice();
        b.accept(&seen(&BOB, "phone", None)).unwrap();
        b.rename(1, "Bob");
        b.flag(0, &MALLORY);
        b.contacts[1].accepted_by_them = Some(true);
        let outbox: Outbox = vec![(BOB, vec![("ab".repeat(16), r#"{"type":"contact"}"#.to_string())]), (ALICE, vec![])];
        let text = to_file(&b, &outbox);
        let (b2, o2) = from_file(&text);
        assert_eq!(b2, b);
        assert_eq!(o2, vec![outbox[0].clone()]);
        assert_eq!(from_file("not json"), (Book::default(), Vec::new()));
        let (partial, _) = from_file(r#"{"contacts":[{"name":"x","key":"zz"},{"name":"y","key":"0101010101010101010101010101010101010101010101010101010101010101"}]}"#);
        assert_eq!(partial.contacts.len(), 1);
        assert_eq!(partial.contacts[0].seen_name, "y");
    }
}
