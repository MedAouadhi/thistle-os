#!/usr/bin/env python3
"""Tailnet chat's reference chat peer: protocol version 1, standard library only.

It listens and dials, holds an identity key, contacts, contact requests, an
outbox and conversations, and is scripted from a test:

    alice, bob = ChatPeer(name="alice"), ChatPeer(name="bob")
    alice.start(); bob.start()
    alice.add_contact(bob.address)                # by address; pinned at first handshake
    mid = alice.send_text(bob.address, "hello")   # into the outbox, dialled now
    bob.wait_for(lambda: bob.requests.get(alice.key_hex))
    bob.accept(alice.key_hex)                     # `contact accepted`; held messages appear
    alice.wait_for(lambda: alice.delivered(mid))

Run as a program it is a peer a test drives over stdin and stdout, one JSON
object a line (docs/tailnet-chat/README.md, "The control protocol"):

    python3 tools/tailnet_chat/peer.py --listen 127.0.0.1:0 --name ref --loopback

docs/tailnet-chat/protocol.md is the authority; where the two differ, this
is wrong.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass, field

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from tailnet_chat import ed25519, protocol, ws
else:
    from . import ed25519, protocol, ws

HANDSHAKE_S = 10.0


def new_id() -> str:
    return os.urandom(16).hex()


def now_ms() -> int:
    return int(time.time() * 1000)


class Refused(Exception):
    """A handshake that did not end in two proven keys, or a peer that refused us."""

    def __init__(self, why: str, code: int | None = None, error: str | None = None):
        super().__init__(why)
        self.why = why
        self.code = code      # the WebSocket close code, when there was one
        self.error = error    # the other side's `error` code, when it sent one


@dataclass
class Message:
    id: str
    mine: bool
    ts: int
    text: str | None = None
    reply_to: str | None = None
    voice: bytes | None = None
    ms: int | None = None
    state: str = "waiting"         # mine: waiting, delivered, read; theirs: received
    reactions: dict = field(default_factory=dict)  # "me"/"them" -> emoji


@dataclass
class Contact:
    name: str
    address: tuple | None = None   # (host, port) to dial
    key: str | None = None         # pinned identity key, hex; None until the first handshake
    tailnet_name: str | None = None
    accepted_by_them: bool | None = None
    declined_by_them: bool = False
    flagged_key: str | None = None  # another key seen under this contact's name or address


@dataclass
class Request:
    key: str
    name: str
    address: tuple
    fingerprint: str
    held: list = field(default_factory=list)  # Message, in arrival order


class Connection:
    """One authenticated or authenticating WebSocket to another chat peer."""

    def __init__(self, peer: "ChatPeer", sock: ws.WebSocket, role: str, remote: tuple,
                 dialled: tuple | None = None):
        self.peer = peer
        self.ws = sock
        self.role = role
        self.remote = remote
        self.dialled = dialled
        self.peer_key: bytes | None = None
        self.peer_name: str | None = None
        self.peer_caps: list = []
        self.authenticated = False
        self.closed = False
        self.close_code: int | None = None
        self.received: list = []      # every frame after the handshake: dicts, or ("binary", bytes)
        self.sent_ids: set = set()
        self.transfers: dict = {}     # stream -> [voice frame, bytearray, next seq]
        self.last_activity = time.monotonic()

    @property
    def peer_key_hex(self) -> str:
        return self.peer_key.hex() if self.peer_key else ""

    @property
    def dialler_key(self) -> bytes:
        return self.peer.key if self.role == "dialler" else self.peer_key

    def send(self, frame: dict) -> None:
        self.last_activity = time.monotonic()
        self.ws.send_text(protocol.encode(frame))

    def send_binary(self, kind: int, stream: int, seq: int, payload: bytes) -> None:
        self.last_activity = time.monotonic()
        self.ws.send_binary(protocol.pack_binary(kind, stream, seq, payload))

    def close(self, code: int = 1000, reason: str = "") -> None:
        if not self.closed:
            self.closed = True
            self.close_code = code
            self.ws.close(code, reason)

    # -- the handshake -----------------------------------------------------

    def handshake(self) -> None:
        """Hello, then auth, both ways; Refused unless both keys are proven."""
        p = self.peer
        nonce = os.urandom(32)
        self.ws.sock.settimeout(HANDSHAKE_S)
        try:
            self.send(protocol.hello(p.key, p.name, p.caps, nonce))
            their = self._expect("hello")
            if their["version"] != protocol.VERSION:
                self._refuse("version", f"version {their['version']}")
            key = bytes.fromhex(their["key"])
            if key == p.key:
                self._refuse("bad_request", "my own key")
            self.peer_key, self.peer_name, self.peer_caps = key, their["name"], their["caps"]
            their_nonce = bytes.fromhex(their["nonce"])
            if self.role == "dialler":
                fields = (nonce, their_nonce, p.key, key)
            else:
                fields = (their_nonce, nonce, key, p.key)
            other = "listener" if self.role == "dialler" else "dialler"
            mine = protocol.transcript(self.role, *fields)
            my_auth = protocol.auth(p.sign_override(mine) if p.sign_override else ed25519.sign(p.seed, mine))
            # The dialler proves itself first; the listener answers only a proven dialler.
            if self.role == "dialler":
                self.send(my_auth)
            sig = bytes.fromhex(self._expect("auth")["sig"])
            if not ed25519.verify(key, protocol.transcript(other, *fields), sig):
                self._refuse("bad_signature", "bad signature")
            if self.role == "listener":
                self.send(my_auth)
        except ws.Closed as e:
            self.close(e.code)
            raise Refused(f"closed during the handshake ({e.code})", e.code)
        except OSError as e:  # the timeout
            self.close(protocol.CLOSE_HANDSHAKE, "handshake timeout")
            raise Refused("handshake timeout: " + str(e))
        self.ws.sock.settimeout(None)
        self.authenticated = True

    def _expect(self, kind: str) -> dict:
        opcode, payload = self.ws.recv()
        if opcode != ws.TEXT:
            self._refuse("bad_request", f"binary before {kind}")
        frame, error = protocol.check(payload)
        if frame and frame["type"] == "error":
            self.close(protocol.CLOSE_HANDSHAKE, frame["code"])
            raise Refused(f"refused by the other side: {frame['code']}", protocol.CLOSE_HANDSHAKE,
                          frame["code"])
        if error or frame["type"] != kind:
            self._refuse("bad_request", f"expected {kind}")
        return frame

    def _refuse(self, code: str, why: str):
        try:
            self.send(protocol.error(code, detail=why))
        except ws.Closed:
            pass
        self.close(protocol.CLOSE_HANDSHAKE, code)
        raise Refused(why)


class ChatPeer:
    def __init__(self, name: str = "reference", seed: bytes | None = None,
                 host: str = "127.0.0.1", port: int = 0, loopback: bool = True,
                 caps=protocol.CAPS_V1, read_receipts: bool = True,
                 call_policy: str = "answer", ping_s: float = 20.0, idle_s: float = 120.0,
                 log=None):
        self.name = name
        self.seed = seed or os.urandom(32)
        self.key = ed25519.public_key(self.seed)
        self.host, self.port = host, port
        self.loopback = loopback
        self.caps = list(caps)
        self.read_receipts = read_receipts
        self.call_policy = call_policy   # answer, decline, busy, ignore
        self.ping_s, self.idle_s = ping_s, idle_s
        self.sign_override = None        # a test may set fn(transcript) -> signature
        self.log_fn = log
        self.cond = threading.Condition()
        self.contacts: list[Contact] = []
        self.requests: dict[str, Request] = {}
        self.conversations: dict[str, list[Message]] = {}
        self.outbox: dict[str, list] = {}  # key hex or "addr:host:port" -> frames (voice: frame, data)
        self.typing: dict[str, float] = {}
        self.calls: dict[str, dict] = {}
        self.errors: list = []           # (key hex, error frame)
        self.events: list = []           # (what, detail) for a test to read
        self.connections: list[Connection] = []
        self.declined_keys: set = set()
        self._listener: socket.socket | None = None
        self._running = False

    # -- identity and lifecycle --------------------------------------------

    @property
    def key_hex(self) -> str:
        return self.key.hex()

    @property
    def fingerprint(self) -> str:
        return protocol.fingerprint(self.key)

    @property
    def address(self) -> tuple:
        return (self.host, self.port)

    def log(self, what: str, detail: str = "") -> None:
        with self.cond:
            self.events.append((what, detail))
            self.cond.notify_all()
        if self.log_fn:
            self.log_fn(f"[{self.name}] {what} {detail}".rstrip())

    def start(self) -> "ChatPeer":
        family = socket.AF_INET6 if ":" in self.host else socket.AF_INET
        s = socket.socket(family, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, self.port))
        s.listen(8)
        self.port = s.getsockname()[1]
        self._listener = s
        self._running = True
        threading.Thread(target=self._accept_loop, daemon=True).start()
        threading.Thread(target=self._ticker, daemon=True).start()
        self.log("listening", f"{self.host}:{self.port} as {self.fingerprint}")
        return self

    def stop(self) -> None:
        self._running = False
        if self._listener:
            # shutdown first: close alone leaves a thread blocked in accept()
            # holding the socket, and the stopped peer answering.
            for step in (lambda: self._listener.shutdown(socket.SHUT_RDWR), self._listener.close):
                try:
                    step()
                except OSError:
                    pass
        for conn in list(self.connections):
            conn.close(1001, "going away")

    def wait_for(self, predicate, timeout: float = 5.0):
        """Wait until `predicate()` is truthy and return it; AssertionError on timeout."""
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                value = predicate()
                if value:
                    return value
                left = deadline - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"{self.name}: timed out waiting; events: {self.events[-8:]}")
                self.cond.wait(min(left, 0.05))

    # -- contacts ----------------------------------------------------------

    def add_contact(self, address=None, name: str | None = None, key: str | None = None) -> Contact:
        """A contact the user chose: by address, by key, or both."""
        address = tuple(address) if address else None
        with self.cond:
            for c in self.contacts:
                if (key and c.key == key) or (address and c.address == address):
                    return c
            c = Contact(name or (f"{address[0]}:{address[1]}" if address else key[:8]),
                        address=address, key=key)
            self.contacts.append(c)
            return c

    def contact(self, who) -> Contact | None:
        """By key hex, (host, port), contact name or tailnet name."""
        with self.cond:
            for c in self.contacts:
                if who in (c.key, c.name, c.tailnet_name) or (isinstance(who, tuple) and c.address == tuple(who)):
                    return c
        return None

    def remove_contact(self, who) -> None:
        with self.cond:
            c = self.contact(who)
            if c:
                self.contacts.remove(c)
                self.outbox.pop(self._box(c), None)

    def accept(self, key_hex: str) -> Contact:
        """Accept a contact request: pin its key, show what it held, tell it."""
        with self.cond:
            req = self.requests.pop(key_hex)
            c = self.contact(key_hex) or Contact(req.name, address=None, key=key_hex)
            c.tailnet_name = req.name
            if c not in self.contacts:
                self.contacts.append(c)
            conv = self.conversations.setdefault(key_hex, [])
            conv.extend(req.held)
            conv.sort(key=lambda m: m.ts)
            self._queue(key_hex, {"type": "contact", "id": new_id(), "state": "accepted"})
        self.log("accepted", key_hex)
        self._flush_async(key_hex)
        return c

    def decline(self, key_hex: str) -> None:
        """Decline a contact request: drop what it held and tell it."""
        with self.cond:
            self.requests.pop(key_hex, None)
            self._queue(key_hex, {"type": "contact", "id": new_id(), "state": "declined"})
        self.log("declined", key_hex)
        self._flush_async(key_hex)

    def accept_new_key(self, who) -> Contact:
        """Put a flagged contact's new key in place of the old one."""
        with self.cond:
            c = self.contact(who)
            old, c.key, c.flagged_key = c.key, c.flagged_key, None
            if old in self.outbox:
                self.outbox[c.key] = self.outbox.pop(old)
            self.conversations[c.key] = self.conversations.pop(old, [])
        self.log("new key accepted", f"{old} -> {c.key}")
        return c

    # -- sending -----------------------------------------------------------

    def _box(self, c: Contact) -> str:
        return c.key if c.key else f"addr:{c.address[0]}:{c.address[1]}"

    def _resolve(self, to) -> Contact:
        c = self.contact(to if not isinstance(to, list) else tuple(to))
        if c is None:
            if isinstance(to, (tuple, list)):
                c = self.add_contact(to)
            else:
                raise KeyError(f"no contact {to!r}")
        if c.declined_by_them:
            raise Refused(f"{c.name} declined our contact request")
        return c

    def _queue(self, box: str, frame: dict, data: bytes | None = None) -> None:
        with self.cond:
            self.outbox.setdefault(box, []).append((frame, data))
            self.cond.notify_all()

    def send_text(self, to, text: str, reply_to: str | None = None, id: str | None = None) -> str:
        c = self._resolve(to)
        frame = {"type": "msg", "id": id or new_id(), "ts": now_ms(), "text": text}
        if reply_to:
            frame["reply_to"] = reply_to
        with self.cond:
            self.conversations.setdefault(self._box(c), []).append(
                Message(frame["id"], True, frame["ts"], text=text, reply_to=reply_to))
            self._queue(self._box(c), frame)
        self._flush_async(c)
        return frame["id"]

    def react(self, to, message_id: str, emoji: str) -> str:
        c = self._resolve(to)
        frame = {"type": "react", "id": new_id(), "ts": now_ms(), "to": message_id, "emoji": emoji}
        with self.cond:
            for m in self.conversations.get(self._box(c), []):
                if m.id == message_id:
                    if emoji:
                        m.reactions["me"] = emoji
                    else:
                        m.reactions.pop("me", None)
            self._queue(self._box(c), frame)
        self._flush_async(c)
        return frame["id"]

    def send_voice(self, to, data: bytes, ms: int, stream: int | None = None) -> str:
        c = self._resolve(to)
        frame = {"type": "voice", "id": new_id(), "ts": now_ms(), "ms": ms, "bytes": len(data),
                 "codec": "ogg-opus", "stream": stream or int.from_bytes(os.urandom(4), "big") | 1}
        with self.cond:
            self.conversations.setdefault(self._box(c), []).append(
                Message(frame["id"], True, frame["ts"], voice=data, ms=ms))
            self._queue(self._box(c), frame, data)
        self._flush_async(c)
        return frame["id"]

    def mark_read(self, who, ids=None) -> None:
        """Read what `who` sent; a `read` goes only if read receipts are on."""
        c = self._resolve(who)
        with self.cond:
            unread = [m for m in self.conversations.get(c.key, []) if not m.mine and m.state == "received"
                      and (ids is None or m.id in ids)]
            for m in unread:
                m.state = "read"
        if unread and self.read_receipts:
            self._send_now(c.key, {"type": "read", "ids": [m.id for m in unread]})

    def set_typing(self, who, on: bool) -> None:
        self._send_now(self._resolve(who).key, {"type": "typing", "on": bool(on)})

    def call(self, who, transports=("ws-opus",)) -> str:
        """Offer a call; the outcome lands in self.calls[call id]["state"]."""
        c = self._resolve(who)
        call_id = new_id()
        stream = int.from_bytes(os.urandom(4), "big") | 1
        with self.cond:
            self.calls[call_id] = {"peer": c.key, "state": "offered", "stream": stream,
                                   "frames": [], "mine": True}
        try:
            conn = self._connection_for(c)
        except (Refused, OSError):
            conn = None
        if conn is None:
            with self.cond:
                self.calls[call_id]["state"] = "ended:unreachable"
                self.cond.notify_all()
            return call_id
        conn.send({"type": "call_offer", "call": call_id, "transports": list(transports), "stream": stream})
        return call_id

    def send_call_audio(self, call_id: str, seq: int, packet: bytes) -> None:
        call = self.calls[call_id]
        conn = self.connected(call["peer"])
        conn.send_binary(protocol.KIND_CALL_AUDIO, call["stream"], seq, packet)

    def end_call(self, call_id: str, reason: str = "hangup") -> None:
        call = self.calls[call_id]
        conn = self.connected(call["peer"])
        if conn:
            conn.send({"type": "call_end", "call": call_id, "reason": reason})
        with self.cond:
            call["state"] = "ended:" + reason
            self.cond.notify_all()

    def _send_now(self, key: str | None, frame: dict) -> bool:
        """Typing and read receipts: sent on an open connection, never queued,
        and only if both hellos announced the capability."""
        conn = self.connected(key) if key else None
        if not conn:
            return False
        cap = {"read": "receipts", "typing": "typing"}.get(frame["type"])
        if cap and not (cap in self.caps and cap in conn.peer_caps):
            return False
        try:
            conn.send(frame)
            return True
        except ws.Closed:
            return False

    # -- reading state -----------------------------------------------------

    def connected(self, key_hex: str) -> Connection | None:
        with self.cond:
            for conn in self.connections:
                if conn.authenticated and not conn.closed and conn.peer_key_hex == key_hex:
                    return conn
        return None

    def messages(self, who) -> list[Message]:
        c = self.contact(who)
        key = self._box(c) if c else who
        with self.cond:
            return list(self.conversations.get(key, []))

    def message(self, message_id: str) -> Message | None:
        with self.cond:
            for conv in self.conversations.values():
                for m in conv:
                    if m.id == message_id:
                        return m
        return None

    def waiting(self, who) -> list[str]:
        """Ids still in the outbox for `who`."""
        c = self.contact(who)
        box = self._box(c) if c else who
        with self.cond:
            return [f["id"] for f, _ in self.outbox.get(box, [])]

    def delivered(self, message_id: str) -> bool:
        m = self.message(message_id)
        return bool(m and m.state in ("delivered", "read"))

    def is_typing(self, key_hex: str) -> bool:
        return self.typing.get(key_hex, 0) > time.monotonic()

    def received(self, key_hex: str, kind: str | None = None) -> list:
        """Every frame received from `key_hex`, on any connection."""
        with self.cond:
            out = []
            for conn in self.connections:
                if conn.peer_key_hex == key_hex:
                    out.extend(f for f in conn.received
                               if kind is None or (isinstance(f, dict) and f.get("type") == kind))
            return out

    # -- connections -------------------------------------------------------

    def connect(self, address, timeout: float = 5.0) -> Connection:
        """Dial `address`, shake hands, and run the connection; Refused if refused."""
        host, port = address
        sock = socket.create_connection((host, port), timeout=timeout)
        try:
            w = ws.client_upgrade(sock, host, port, protocol.PATH)
        except ws.Closed as e:
            sock.close()
            raise Refused(str(e))
        conn = Connection(self, w, "dialler", (host, port), dialled=(host, port))
        conn.handshake()
        if not self._admit_authenticated(conn):
            raise Refused(f"refused {conn.peer_key_hex[:16]}", conn.close_code)
        threading.Thread(target=self._read_loop, args=(conn,), daemon=True).start()
        return conn

    def _connection_for(self, c: Contact) -> Connection | None:
        if c.key:
            conn = self.connected(c.key)
            if conn:
                return conn
        if not c.address:
            return None
        return self.connect(c.address)

    def flush(self, who=None) -> None:
        """Dial and send the outbox now: for `who`, or for everyone with something waiting."""
        targets = [who] if who is not None else list(self.outbox)
        for t in targets:
            c = t if isinstance(t, Contact) else self.contact(t)
            key = c.key if c else t
            try:
                conn = self._connection_for(c) if c else self.connected(key)
            except (Refused, OSError) as e:
                self.log("unreachable", f"{key}: {e}")
                continue
            if conn:
                self._flush(conn)

    def _flush_async(self, who) -> None:
        threading.Thread(target=self.flush, args=(who,), daemon=True).start()

    def _flush(self, conn: Connection) -> None:
        with self.cond:
            box = self.outbox.get(conn.peer_key_hex, [])
            todo = [(f, d) for f, d in box if f["id"] not in conn.sent_ids]
            for f, _ in todo:
                conn.sent_ids.add(f["id"])
        for frame, data in todo:
            try:
                conn.send(frame)
                if data is not None:
                    for seq, at in enumerate(range(0, len(data), protocol.MAX_CHUNK)):
                        conn.send_binary(protocol.KIND_VOICE, frame["stream"], seq,
                                         data[at:at + protocol.MAX_CHUNK])
            except ws.Closed:
                return

    def _accept_loop(self) -> None:
        while self._running:
            try:
                sock, remote = self._listener.accept()
            except OSError:
                return
            if not self._running:
                sock.close()
                return
            threading.Thread(target=self._serve, args=(sock, remote), daemon=True).start()

    def _serve(self, sock: socket.socket, remote) -> None:
        if not protocol.admitted(remote[0], loopback=self.loopback):
            self.log("not admitted", remote[0])
            sock.close()
            return
        sock.settimeout(HANDSHAKE_S)
        try:
            w = ws.server_upgrade(sock, protocol.PATH)
        except (ws.Closed, OSError):
            sock.close()
            return
        conn = Connection(self, w, "listener", (remote[0], remote[1]))
        try:
            conn.handshake()
        except Refused as e:
            self.log("refused", f"{remote[0]}: {e.why}")
            return
        if self._admit_authenticated(conn):
            self._read_loop(conn)

    def _admit_authenticated(self, conn: Connection) -> bool:
        """Who it proved to be: a contact, a contact's impostor, or a stranger."""
        key = conn.peer_key_hex
        changed = pinned = None
        with self.cond:
            contact = self.contact(key)
            for c in self.contacts if contact is None else ():
                # An incoming connection's source address says nothing on loopback,
                # where every test peer shares one.
                by_source = (conn.role == "listener" and c.address is not None
                             and c.address[0] == conn.remote[0]
                             and protocol.admitted(conn.remote[0]))
                by_address = (conn.dialled is not None and c.address == conn.dialled) or by_source
                by_name = c.tailnet_name is not None and c.tailnet_name == conn.peer_name
                if (by_address or by_name) and c.key and c.key != key:
                    c.flagged_key = key
                    changed = c
                    break
                if by_address and c.key is None:
                    # Dialled by the address the user gave: the tailnet is trusted once.
                    c.key = key
                    old = f"addr:{c.address[0]}:{c.address[1]}"
                    if old in self.outbox:
                        self.outbox.setdefault(key, []).extend(self.outbox.pop(old))
                    if old in self.conversations:
                        self.conversations.setdefault(key, []).extend(self.conversations.pop(old))
                    contact = pinned = c
                    break
            if contact is not None and contact.tailnet_name is None:
                contact.tailnet_name = conn.peer_name
            # One connection per pair: keep the one dialled by the lower key.
            other = self.connected(key)
            self.connections.append(conn)
            self.cond.notify_all()
        if pinned is not None:
            self.log("pinned", f"{pinned.name} as {protocol.fingerprint(conn.peer_key)}")
        if changed is not None:
            self.log("key changed", f"{changed.name}: {key}")
            try:
                conn.send(protocol.error("key_changed", detail=changed.name))
            except ws.Closed:
                pass
            conn.close(protocol.CLOSE_KEY_CHANGED, "key_changed")
            return False
        if other is not None:
            loser = conn if other.dialler_key < conn.dialler_key else other
            self.log("duplicate", f"closing the one dialled by {loser.dialler_key.hex()[:8]}")
            loser.close(protocol.CLOSE_DUPLICATE, "duplicate")
            if loser is conn:
                return False
        who = "contact " + contact.name if contact else "stranger"
        self.log("connected", f"{conn.role} {conn.peer_name} {protocol.fingerprint(conn.peer_key)} ({who})")
        threading.Thread(target=self._flush, args=(conn,), daemon=True).start()
        return True

    def _read_loop(self, conn: Connection) -> None:
        try:
            while True:
                opcode, payload = conn.ws.recv()
                conn.last_activity = time.monotonic()
                if opcode == ws.BINARY:
                    self._on_binary(conn, payload)
                else:
                    self._on_text(conn, payload)
        except ws.Closed as e:
            conn.closed = True
            conn.close_code = conn.close_code or e.code
        finally:
            conn.transfers.clear()  # a broken voice transfer starts again next time
            with self.cond:
                self.cond.notify_all()
            self.log("disconnected", conn.peer_key_hex[:16])

    # -- receiving ---------------------------------------------------------

    def _reply(self, conn: Connection, frame: dict) -> None:
        try:
            conn.send(frame)
        except ws.Closed:
            pass

    def _on_text(self, conn: Connection, payload: bytes) -> None:
        frame, error = protocol.check(payload)
        if error:
            with self.cond:
                conn.received.append({"type": "_invalid", "text": payload.decode("utf-8", "replace")})
            self.log("answered", f"error {error['code']} {error.get('what', '')}")
            self._reply(conn, error)
            return
        key = conn.peer_key_hex
        with self.cond:
            conn.received.append(frame)
            self.cond.notify_all()
        kind = frame["type"]
        handler = getattr(self, "_on_" + kind, None)
        if kind in ("hello", "auth") or handler is None:
            self._reply(conn, protocol.error("bad_request", what=kind, detail="after the handshake"))
            return
        handler(conn, key, frame)

    def _is_contact(self, key: str) -> Contact | None:
        return self.contact(key)

    def _store(self, conn: Connection, key: str, m: Message, frame: dict) -> None:
        """A message, reaction or voice message received: store once, ack every time."""
        with self.cond:
            contact = self._is_contact(key)
            if contact is not None:
                conv = self.conversations.setdefault(key, [])
                seen = any(x.id == m.id for x in conv)
            else:
                req = self.requests.get(key)
                if req is None:
                    req = Request(key, conn.peer_name, conn.remote, protocol.fingerprint(conn.peer_key))
                    self.requests[key] = req
                    self.log("contact request", f"{conn.peer_name} {req.fingerprint}")
                conv = req.held
                seen = any(x.id == m.id for x in conv)
            if not seen:
                conv.append(m)
                conv.sort(key=lambda x: x.ts)
            self.cond.notify_all()
        self.log("duplicate" if seen else "received", f"{frame['type']} {m.id}")
        self._reply(conn, {"type": "ack", "id": frame["id"]})

    def _on_msg(self, conn, key, frame):
        self._store(conn, key, Message(frame["id"], False, frame["ts"], text=frame["text"],
                                       reply_to=frame.get("reply_to"), state="received"), frame)

    def _on_react(self, conn, key, frame):
        with self.cond:
            seen = any(m.id == frame["id"] for conv in [self.conversations.get(key, [])] for m in conv)
            target = None
            for m in self.conversations.get(key, []):
                if m.id == frame["to"]:
                    target = m
            if target is not None and not seen:
                if frame["emoji"]:
                    target.reactions["them"] = frame["emoji"]
                else:
                    target.reactions.pop("them", None)
        # The reaction itself is kept like a message, so a duplicate is acked and not applied twice.
        m = Message(frame["id"], False, frame["ts"], text=None, state="received")
        m.reactions = {"to": frame["to"], "emoji": frame["emoji"]}
        self._store(conn, key, m, frame)

    def _on_ack(self, conn, key, frame):
        with self.cond:
            box = self.outbox.get(key, [])
            self.outbox[key] = [(f, d) for f, d in box if f["id"] != frame["id"]]
            m = self.message(frame["id"])
            if m and m.mine and m.state == "waiting":
                m.state = "delivered"
            self.cond.notify_all()
        self.log("delivered", frame["id"])

    def _on_read(self, conn, key, frame):
        with self.cond:
            for m in self.conversations.get(key, []):
                if m.mine and m.id in frame["ids"]:
                    m.state = "read"
            self.cond.notify_all()

    def _on_typing(self, conn, key, frame):
        with self.cond:
            self.typing[key] = time.monotonic() + 6 if frame["on"] else 0
            self.cond.notify_all()

    def _on_contact(self, conn, key, frame):
        with self.cond:
            c = self.contact(key)
            if c is not None:
                if frame["state"] == "accepted":
                    c.accepted_by_them = True
                else:
                    c.accepted_by_them = False
                    c.declined_by_them = True
                    self.outbox.pop(key, None)
            self.cond.notify_all()
        self.log("contact " + frame["state"], key)
        self._reply(conn, {"type": "ack", "id": frame["id"]})

    def _on_voice(self, conn, key, frame):
        conn.transfers[frame["stream"]] = [frame, bytearray(), 0]

    def _on_binary(self, conn: Connection, payload: bytes) -> None:
        try:
            kind, stream, seq, data = protocol.unpack_binary(payload)
        except ValueError as e:
            self._reply(conn, protocol.error("bad_request", what="binary", detail=str(e)))
            return
        with self.cond:
            conn.received.append(("binary", payload))
            self.cond.notify_all()
        if kind == protocol.KIND_VOICE:
            t = conn.transfers.get(stream)
            if t is None or seq != t[2]:
                conn.transfers.pop(stream, None)
                self._reply(conn, protocol.error("bad_request", what="binary",
                                                 detail=f"no voice transfer at stream {stream} seq {seq}"))
                return
            frame, buf, _ = t
            buf.extend(data)
            t[2] += 1
            if len(buf) > frame["bytes"]:
                conn.transfers.pop(stream, None)
                self._reply(conn, protocol.error("bad_request", what="voice", id=frame["id"],
                                                 detail="more bytes than announced"))
            elif len(buf) == frame["bytes"]:
                conn.transfers.pop(stream, None)
                self._store(conn, conn.peer_key_hex,
                            Message(frame["id"], False, frame["ts"], voice=bytes(buf), ms=frame["ms"],
                                    reply_to=frame.get("reply_to"), state="received"), frame)
        elif kind == protocol.KIND_CALL_AUDIO:
            with self.cond:
                for call in self.calls.values():
                    if call["stream"] == stream and call["state"] == "connected":
                        call["frames"].append((seq, data))
                        call["last_frame"] = time.monotonic()
                self.cond.notify_all()
        else:
            self._reply(conn, protocol.error("unsupported", what=f"binary kind {kind}"))

    def _on_call_offer(self, conn, key, frame):
        call_id = frame["call"]
        with self.cond:
            busy = any(c["state"] in ("connected", "offered", "ringing") for c in self.calls.values())
            self.calls[call_id] = {"peer": key, "state": "ringing", "stream": frame["stream"],
                                   "frames": [], "mine": False, "transports": frame["transports"]}
            self.cond.notify_all()
        if not self.contact(key):
            answer = {"decline": "declined"}
        elif busy:
            answer = {"decline": "busy"}
        elif "ws-opus" not in frame["transports"] or "call:ws-opus" not in self.caps:
            answer = {"decline": "unsupported"}
        elif self.call_policy == "answer":
            answer = {"transport": "ws-opus"}
        elif self.call_policy in ("decline", "busy"):
            answer = {"decline": "declined" if self.call_policy == "decline" else "busy"}
        else:
            return  # ringing; a test answers with answer_call()
        self._answer(conn, call_id, answer)

    def answer_call(self, call_id: str, transport: str | None = "ws-opus", decline: str | None = None) -> None:
        conn = self.connected(self.calls[call_id]["peer"])
        self._answer(conn, call_id, {"decline": decline} if decline else {"transport": transport})

    def _answer(self, conn, call_id, answer):
        with self.cond:
            self.calls[call_id]["state"] = "connected" if "transport" in answer else "declined:" + answer["decline"]
            self.calls[call_id]["last_frame"] = time.monotonic()
            self.cond.notify_all()
        self._reply(conn, dict({"type": "call_answer", "call": call_id}, **answer))

    def _on_call_answer(self, conn, key, frame):
        with self.cond:
            call = self.calls.get(frame["call"])
            if call:
                call["state"] = "connected" if "transport" in frame else "declined:" + frame["decline"]
                call["last_frame"] = time.monotonic()
            self.cond.notify_all()

    def _on_call_end(self, conn, key, frame):
        with self.cond:
            call = self.calls.get(frame["call"])
            if call:
                call["state"] = "ended:" + frame["reason"]
            self.cond.notify_all()

    def _on_error(self, conn, key, frame):
        with self.cond:
            self.errors.append((key, frame))
            self.cond.notify_all()
        self.log("error from peer", f"{frame['code']} {frame.get('what', '')}")

    # -- liveness ----------------------------------------------------------

    def _ticker(self) -> None:
        last_ping = time.monotonic()
        while self._running:
            time.sleep(0.2)
            now = time.monotonic()
            with self.cond:
                conns = [c for c in self.connections if c.authenticated and not c.closed]
                in_call = {c["peer"] for c in self.calls.values() if c["state"] == "connected"}
                for call_id, call in self.calls.items():
                    if call["state"] == "connected" and now - call.get("last_frame", now) > 10:
                        call["state"] = "ended:timeout"
                        self.cond.notify_all()
            if now - last_ping >= self.ping_s:
                last_ping = now
                for c in conns:
                    try:
                        c.ws.ping()
                    except ws.Closed:
                        pass
            for c in conns:
                busy = self.outbox.get(c.peer_key_hex) or c.peer_key_hex in in_call or c.transfers
                if not busy and now - c.last_activity > self.idle_s:
                    c.close(protocol.CLOSE_IDLE, "idle")


# -- the control protocol: a peer a test drives over stdin and stdout ----------

def _message_json(m: Message) -> dict:
    out = {"id": m.id, "mine": m.mine, "ts": m.ts, "state": m.state}
    if m.text is not None:
        out["text"] = m.text
    if m.reply_to:
        out["reply_to"] = m.reply_to
    if m.voice is not None:
        out["voice_bytes"] = len(m.voice)
        out["ms"] = m.ms
    if m.reactions:
        out["reactions"] = m.reactions
    return out


def control(peer: ChatPeer, command: dict):
    """One control command; returns its result (JSON-able)."""
    cmd = command["cmd"]
    to = command.get("to")
    if isinstance(to, list):
        to = tuple(to)
    if cmd == "identity":
        return {"key": peer.key_hex, "fingerprint": peer.fingerprint, "name": peer.name,
                "address": list(peer.address)}
    if cmd == "add_contact":
        c = peer.add_contact(command.get("address"), command.get("name"), command.get("key"))
        return {"name": c.name}
    if cmd == "contacts":
        return [{"name": c.name, "key": c.key, "address": list(c.address) if c.address else None,
                 "tailnet_name": c.tailnet_name, "flagged_key": c.flagged_key,
                 "accepted_by_them": c.accepted_by_them, "declined_by_them": c.declined_by_them}
                for c in peer.contacts]
    if cmd == "requests":
        return [{"key": r.key, "name": r.name, "fingerprint": r.fingerprint,
                 "held": [_message_json(m) for m in r.held]} for r in peer.requests.values()]
    if cmd == "accept":
        peer.accept(command["key"])
        return True
    if cmd == "decline":
        peer.decline(command["key"])
        return True
    if cmd == "accept_new_key":
        peer.accept_new_key(command["contact"])
        return True
    if cmd == "send_text":
        return peer.send_text(to, command["text"], command.get("reply_to"))
    if cmd == "react":
        return peer.react(to, command["message"], command["emoji"])
    if cmd == "send_voice":
        with open(command["file"], "rb") as f:
            return peer.send_voice(to, f.read(), command["ms"])
    if cmd == "mark_read":
        peer.mark_read(to, command.get("ids"))
        return True
    if cmd == "typing":
        peer.set_typing(to, command["on"])
        return True
    if cmd == "set":
        for name in ("read_receipts", "call_policy"):
            if name in command:
                setattr(peer, name, command[name])
        return True
    if cmd == "messages":
        return [_message_json(m) for m in peer.messages(to)]
    if cmd == "waiting":
        return peer.waiting(to)
    if cmd == "is_typing":
        return peer.is_typing(command["key"])
    if cmd == "flush":
        peer.flush(to)
        return True
    if cmd == "calls":
        return {k: {"peer": v["peer"], "state": v["state"], "frames": len(v["frames"])}
                for k, v in peer.calls.items()}
    if cmd == "call":
        return peer.call(to)
    if cmd == "end_call":
        peer.end_call(command["call"], command.get("reason", "hangup"))
        return True
    if cmd == "stop":
        peer.stop()
        return True
    raise KeyError(f"no command {cmd!r}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--listen", default=f"127.0.0.1:{protocol.PORT}", help="HOST:PORT (port 0: any)")
    ap.add_argument("--name", default="reference")
    ap.add_argument("--seed", help="the identity key's seed, 64 hex digits (default: random)")
    ap.add_argument("--loopback", action="store_true", help="admit loopback, as the simulator does")
    ap.add_argument("--call-policy", default="answer", choices=("answer", "decline", "busy", "ignore"))
    ap.add_argument("--no-read-receipts", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="log to stderr")
    args = ap.parse_args(argv)
    host, _, port = args.listen.rpartition(":")
    peer = ChatPeer(name=args.name, seed=bytes.fromhex(args.seed) if args.seed else None,
                    host=host.strip("[]"), port=int(port), loopback=args.loopback,
                    read_receipts=not args.no_read_receipts, call_policy=args.call_policy,
                    log=(lambda line: print(line, file=sys.stderr, flush=True)) if args.verbose else None)
    peer.start()
    print(json.dumps({"ready": control(peer, {"cmd": "identity"})}), flush=True)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            command = json.loads(line)
            reply = {"ok": control(peer, command)}
        except Exception as e:  # noqa: BLE001 - the driver gets every failure as data
            reply = {"error": f"{type(e).__name__}: {e}"}
        if isinstance(command, dict) and "seq" in command:
            reply["seq"] = command["seq"]
        print(json.dumps(reply, ensure_ascii=False), flush=True)
        if isinstance(command, dict) and command.get("cmd") == "stop":
            break
    peer.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
