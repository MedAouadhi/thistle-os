#!/usr/bin/env python3
"""Tailnet chat's conformance suite: protocol version 1 scenarios for any chat peer.

Each scenario drives the target through its outside, the protocol on the
wire, from a raw dialler (`RawConn`) or a reference chat peer, and through
a `Target`: the target's address and key, plus the few things only its user
can do (accept a contact request, send a message) and read (its requests,
its conversation). A target is:

  - `ReferenceTarget()`: a reference chat peer in this process;
  - `ProcessTarget(argv)`: any program that speaks the control protocol on
    stdin and stdout (docs/tailnet-chat/README.md), as peer.py does and the
    companion app's headless core will;
  - a class of a later ticket's own, for the simulator, implementing
    `Target`'s methods with its keys, screen and card.

    python3 tools/tailnet_chat/conformance.py --reference
    python3 tools/tailnet_chat/conformance.py --control 'python3 tools/tailnet_chat/peer.py --listen 127.0.0.1:0 --loopback'
    python3 tools/tailnet_chat/conformance.py --address 127.0.0.1:4810 --key HEX   # wire-only scenarios
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import threading
import time
import traceback

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from tailnet_chat import ed25519, protocol, ws
    from tailnet_chat.peer import ChatPeer, Refused, control, new_id, now_ms
else:
    from . import ed25519, protocol, ws
    from .peer import ChatPeer, Refused, control, new_id, now_ms

VECTORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "tailnet-chat", "vectors")


def eventually(fn, timeout: float = 5.0, what: str = "a condition"):
    deadline = time.monotonic() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.05)


# -- targets -----------------------------------------------------------------

class Target:
    """What a scenario needs of the chat peer under test.

    `address` and `key` are enough for the wire-only scenarios; the rest
    are its user's actions and what its screen shows, through `call`."""

    address: tuple
    key: str
    wire_only = False

    def call(self, command: dict):
        raise NotImplementedError

    def add_contact(self, address, name=None):
        return self.call({"cmd": "add_contact", "address": list(address), "name": name})

    def contacts(self) -> list:
        return self.call({"cmd": "contacts"})

    def requests(self) -> list:
        return self.call({"cmd": "requests"})

    def accept(self, key: str):
        return self.call({"cmd": "accept", "key": key})

    def decline(self, key: str):
        return self.call({"cmd": "decline", "key": key})

    def accept_new_key(self, contact: str):
        return self.call({"cmd": "accept_new_key", "contact": contact})

    def messages(self, who) -> list:
        return self.call({"cmd": "messages", "to": who})

    def send_text(self, to, text: str, reply_to: str | None = None) -> str:
        command = {"cmd": "send_text", "to": list(to) if isinstance(to, tuple) else to, "text": text}
        if reply_to:
            command["reply_to"] = reply_to
        return self.call(command)

    def waiting(self, who) -> list:
        return self.call({"cmd": "waiting", "to": list(who) if isinstance(who, tuple) else who})

    # Messages, receipts, typing, reactions and the outbox (tickets 04 and 05).

    def react(self, to, message: str, emoji: str) -> str:
        return self.call({"cmd": "react", "to": to, "message": message, "emoji": emoji})

    def mark_read(self, to, ids=None):
        return self.call({"cmd": "mark_read", "to": to, "ids": ids})

    def set_typing(self, to, on: bool):
        return self.call({"cmd": "typing", "to": to, "on": on})

    def is_typing(self, key: str) -> bool:
        return self.call({"cmd": "is_typing", "key": key})

    def set_read_receipts(self, on: bool):
        return self.call({"cmd": "set", "read_receipts": on})

    def flush(self, who=None):
        """Look for `who` now, as when it is seen online, and send what waits for it."""
        return self.call({"cmd": "flush", "to": list(who) if isinstance(who, tuple) else who})

    # Voice messages and calls (ticket 10; ticket 09 runs them against the simulator).

    def send_voice(self, to, path: str, ms: int) -> str:
        return self.call({"cmd": "send_voice", "to": to, "file": path, "ms": ms})

    def set_call_policy(self, policy: str):
        """`ignore`: let an offer ring for the user; `answer`: take every call.
        A target whose calls always ring for its user (the simulator) need
        only accept `ignore`."""
        return self.call({"cmd": "set", "call_policy": policy})

    def place_call(self, to, video: bool = False) -> str:
        command = {"cmd": "call", "to": list(to) if isinstance(to, tuple) else to}
        if video:
            command["video"] = True
        return self.call(command)

    # WebRTC calls (protocol section 4.11; tailnet-video ticket 03). What the
    # target's WebRTC would do (its SDP, its candidates, its camera, its
    # connection's state) goes through the control protocol.

    def set_webrtc(self, on: bool):
        """Announce `call:webrtc` in hellos from now on, or not. A target
        that is not video-capable (the device) takes False and raises
        Skipped for True."""
        return self.call({"cmd": "set", "webrtc": on})

    def rtc(self, call: str, sdp: dict | None = None, candidate: dict | None = None) -> bool:
        """The target's WebRTC gave this SDP or candidate: whether it was sent."""
        command = {"cmd": "rtc", "call": call}
        command.update({"sdp": sdp} if sdp is not None else {"candidate": candidate})
        return self.call(command)

    def rtc_received(self, call: str) -> list:
        """What the target took for its WebRTC, in order: [{"sdp"} or {"candidate"}]."""
        return self.call({"cmd": "rtc_received", "call": call})

    def camera(self, call: str, camera: str) -> bool:
        return self.call({"cmd": "camera", "call": call, "camera": camera})

    def media(self, call: str, state: str):
        """The target's WebRTC connection is `connected`, `disconnected` or `failed`."""
        return self.call({"cmd": "media", "call": call, "state": state})

    def calls(self) -> dict:
        return self.call({"cmd": "calls"})

    def answer_call(self, call: str, decline: str | None = None):
        command = {"cmd": "answer_call", "call": call}
        if decline:
            command["decline"] = decline
        return self.call(command)

    def end_call(self, call: str, reason: str = "hangup"):
        return self.call({"cmd": "end_call", "call": call, "reason": reason})

    def talk(self, call: str):
        """Have the target send audio on a connected call for about a second.
        A target whose microphone streams by itself (the headless core, the
        simulator) answers `true` and does nothing more."""
        return self.call({"cmd": "talk", "call": call})

    def cellular_call(self):
        """A cellular call takes the device over. A target with no cellular
        modem raises Skipped (`can_cellular` False)."""
        if not self.can_cellular:
            raise Skipped("the target has no cellular calls")
        return self.call({"cmd": "cellular_call"})

    can_cellular = True

    # Sharing the tailnet's peer list (ticket 14).

    def share_peers(self, peers: list) -> list:
        """Give the target `peers` as its tailnet's online peers, where it
        can, and return what it shares with a contact at an address outside
        them. A target whose platform has a peer list of its own (the
        simulator) keeps it and returns what it shares of that."""
        return self.call({"cmd": "set_peers", "peers": peers})

    def ask_peers(self, to) -> list:
        """Have the target ask its contact `to` for its peer list. A target
        that never asks (the device) raises Skipped."""
        try:
            return self.call({"cmd": "ask_peers", "to": list(to) if isinstance(to, tuple) else to})
        except RuntimeError as e:
            if "no command" in str(e) or "not on this device" in str(e):
                raise Skipped("the target does not ask for peers")
            raise

    def stop(self):
        pass


class Skipped(Exception):
    """A scenario the target cannot do, such as a cellular call without a modem."""


class ReferenceTarget(Target):
    def __init__(self, name: str = "target", **kwargs):
        self.peer = ChatPeer(name=name, **kwargs).start()
        self.address = self.peer.address
        self.key = self.peer.key_hex

    def call(self, command: dict):
        return control(self.peer, command)

    def stop(self):
        self.peer.stop()


class ProcessTarget(Target):
    """A chat peer in another process, driven by the control protocol."""

    def __init__(self, argv, timeout: float = 10.0):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, encoding="utf-8", bufsize=1)
        self.timeout = timeout
        self.seq = 0
        self.lock = threading.Lock()
        ready = self._read()
        if "ready" not in ready:
            raise RuntimeError(f"the target did not say ready: {ready}")
        self.key = ready["ready"]["key"]
        self.address = tuple(ready["ready"]["address"])

    def _read(self) -> dict:
        line = []

        def read():
            line.append(self.proc.stdout.readline())
        t = threading.Thread(target=read, daemon=True)
        t.start()
        t.join(self.timeout)
        if not line or not line[0]:
            raise RuntimeError("the target process said nothing")
        return json.loads(line[0])

    def call(self, command: dict):
        with self.lock:
            self.seq += 1
            command = dict(command, seq=self.seq)
            self.proc.stdin.write(json.dumps(command, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
            while True:
                reply = self._read()
                if reply.get("seq") == self.seq:
                    break
        if "error" in reply:
            if command["cmd"] == "cellular_call" and "no command" in reply["error"]:
                raise Skipped("the target has no cellular calls")
            raise RuntimeError(f"{command['cmd']}: {reply['error']}")
        return reply["ok"]

    def stop(self):
        try:
            self.call({"cmd": "stop"})
        except Exception:  # noqa: BLE001
            pass
        try:
            self.proc.stdin.close()
            self.proc.wait(5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
            self.proc.wait(5)
        self.proc.stdout.close()


class AddressTarget(Target):
    """Only an address and a key: the wire-only scenarios."""
    wire_only = True

    def __init__(self, address, key: str):
        self.address = tuple(address)
        self.key = key


# -- a raw dialler -------------------------------------------------------------

class RawConn:
    """A dialler that does each step of the protocol by hand, right or wrong."""

    def __init__(self, address, seed: bytes | None = None, name: str = "conformance",
                 caps=protocol.CAPS_V1):
        self.address = tuple(address)
        self.seed = seed or os.urandom(32)
        self.key = ed25519.public_key(self.seed)
        self.name = name
        self.caps = list(caps)
        self.nonce = os.urandom(32)
        self.their = None
        self.ws = None

    @property
    def key_hex(self) -> str:
        return self.key.hex()

    def open(self) -> "RawConn":
        sock = socket.create_connection(self.address, timeout=5)
        self.ws = ws.client_upgrade(sock, self.address[0], self.address[1], protocol.PATH)
        return self

    def hello(self, **override) -> dict:
        frame = protocol.hello(self.key, self.name, self.caps, self.nonce)
        frame.update(override)
        self.send(frame)
        self.their = self.expect("hello")
        return self.their

    def transcript(self, role: str) -> bytes:
        return protocol.transcript(role, self.nonce, bytes.fromhex(self.their["nonce"]),
                                   self.key, bytes.fromhex(self.their["key"]))

    def auth(self, sig: bytes | None = None) -> None:
        self.send(protocol.auth(sig if sig is not None else ed25519.sign(self.seed, self.transcript("dialler"))))

    def handshake(self) -> "RawConn":
        self.open()
        self.hello()
        self.auth()
        theirs = self.expect("auth")
        assert ed25519.verify(bytes.fromhex(self.their["key"]), self.transcript("listener"),
                              bytes.fromhex(theirs["sig"])), "the target's auth does not verify"
        return self

    def send(self, frame) -> None:
        self.ws.send_text(frame if isinstance(frame, str) else protocol.encode(frame))

    def send_binary(self, data: bytes) -> None:
        self.ws.send_binary(data)

    def recv(self, timeout: float = 5.0):
        self.ws.sock.settimeout(timeout)
        try:
            opcode, payload = self.ws.recv()
        except socket.timeout:
            raise AssertionError("nothing received in time")
        if opcode == ws.BINARY:
            return ("binary", payload)
        frame, error = protocol.check(payload)
        assert error is None, f"the target sent a frame that does not check: {payload!r}: {error}"
        return frame

    def expect(self, kind: str, timeout: float = 5.0, **fields) -> dict:
        """The next frame of `kind` with `fields`; others in between are skipped, but
        an `error` is never skipped unless it is what is expected."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self.recv(max(0.05, deadline - time.monotonic()))
            if isinstance(frame, dict) and frame["type"] == kind and all(frame.get(k) == v for k, v in fields.items()):
                return frame
            if isinstance(frame, dict) and frame["type"] == "error":
                raise AssertionError(f"expected {kind} {fields}, got {frame}")

    def drain(self, seconds: float) -> list:
        """Every frame received in the next `seconds`."""
        deadline, out = time.monotonic() + seconds, []
        while time.monotonic() < deadline:
            try:
                out.append(self.recv(max(0.05, deadline - time.monotonic())))
            except AssertionError:
                break
        return out

    def expect_closed(self, timeout: float = 5.0) -> int:
        """Read until the target closes; its close code (1006: dropped without one)."""
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                self.recv(max(0.05, deadline - time.monotonic()))
        except ws.Closed as e:
            self.close()
            return e.code
        raise AssertionError("the target did not close the connection")

    def close(self) -> None:
        if self.ws:
            self.ws.close()


def _msg(text: str = "hello", **extra) -> dict:
    frame = {"type": "msg", "id": new_id(), "ts": now_ms(), "text": text}
    frame.update(extra)
    return frame


def _request_from(target: Target, key: str):
    for r in target.requests():
        if r["key"] == key:
            return r
    return None


def _shown(target: Target, key: str) -> list:
    return [m for m in target.messages(key) if not m["mine"] and m.get("text") is not None]


def _become_contact(target: Target, conn: RawConn) -> None:
    """A request from `conn`, accepted by the target's user."""
    m = _msg("may I?")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    eventually(lambda: _request_from(target, conn.key_hex), what="the contact request")
    target.accept(conn.key_hex)
    accepted = conn.expect("contact", state="accepted")
    conn.send({"type": "ack", "id": accepted["id"]})


# -- the scenarios -------------------------------------------------------------
# Each takes a Target, raises AssertionError on a failure, and leaves the
# target as it found it apart from its contacts and conversations.

def handshake(target: Target) -> None:
    """hello and auth both ways; the target proves the key it announced."""
    conn = RawConn(target.address).open()
    their = conn.hello()
    assert their["version"] == 1, their
    assert their["key"] == target.key, "the target's hello announced another key"
    for cap in ("text", "receipts"):
        assert cap in their["caps"], f"no {cap} in {their['caps']}"
    conn.auth()
    theirs = conn.expect("auth")
    assert ed25519.verify(bytes.fromhex(their["key"]), conn.transcript("listener"), bytes.fromhex(theirs["sig"]))
    conn.close()


def bad_signature_refused(target: Target) -> None:
    """A wrong signature, or one over the wrong role's transcript, closes the connection."""
    for why, make in (
        ("a corrupt signature", lambda c: bytes(64)),
        ("the listener's transcript signed", lambda c: ed25519.sign(c.seed, c.transcript("listener"))),
        ("another key's signature", lambda c: ed25519.sign(os.urandom(32), c.transcript("dialler"))),
    ):
        conn = RawConn(target.address).open()
        conn.hello()
        conn.auth(make(conn))
        seen = []
        try:
            conn.ws.sock.settimeout(5)
            while True:
                opcode, payload = conn.ws.recv()
                seen.append(protocol.check(payload)[0] if opcode == ws.TEXT else ("binary", payload))
        except ws.Closed as e:
            code = e.code
        except TimeoutError:
            raise AssertionError(f"{why}: the target did not close the connection")
        conn.close()
        assert code in (protocol.CLOSE_HANDSHAKE, 1006), f"{why}: closed with {code}"
        kinds = [f.get("type") for f in seen if isinstance(f, dict)]
        assert "auth" not in kinds, f"{why}: the target sent its auth to an unproven dialler"
        assert set(kinds) <= {"error"}, f"{why}: the target sent {kinds}"


def unknown_type_answered(target: Target) -> None:
    """An unknown type draws `error unsupported` naming it, and the connection stays open."""
    conn = RawConn(target.address).handshake()
    conn.send({"type": "sticker", "id": new_id()})
    err = conn.expect("error", code="unsupported")
    assert err.get("what") == "sticker", err
    m = _msg("still here")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    conn.close()


def invalid_frames_answered(target: Target) -> None:
    """Every invalid frame of the vectors draws its error, the connection still open."""
    with open(os.path.join(VECTORS, "messages.json"), encoding="utf-8") as f:
        invalid = json.load(f)["invalid"]
    conn = RawConn(target.address).handshake()
    for example in invalid:
        conn.send(example["text"])
        err = conn.expect("error", code=example["error"]["code"])
        for name in ("what", "id"):
            if name in example["error"]:
                assert err.get(name) == example["error"][name], (example["name"], err)
    m = _msg("still here")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    conn.close()


def contact_request_accepted(target: Target) -> None:
    """A stranger's messages are acked and held; accepting tells it, and they appear."""
    conn = RawConn(target.address, name="stranger").handshake()
    first, second = _msg("first"), _msg("second")
    for m in (first, second):
        conn.send(m)
        conn.expect("ack", id=m["id"])
    req = eventually(lambda: _request_from(target, conn.key_hex), what="the contact request")
    assert req["fingerprint"] == protocol.fingerprint(conn.key), req
    assert _shown(target, conn.key_hex) == [], "a stranger's message was shown before acceptance"
    target.accept(conn.key_hex)
    accepted = conn.expect("contact", state="accepted")
    conn.send({"type": "ack", "id": accepted["id"]})
    shown = eventually(lambda: _shown(target, conn.key_hex), what="the held messages")
    assert [m["text"] for m in shown] == ["first", "second"], shown
    assert not _request_from(target, conn.key_hex)
    conn.close()


def contact_request_declined(target: Target) -> None:
    """Declining tells the stranger and drops what it sent."""
    conn = RawConn(target.address, name="unwanted").handshake()
    m = _msg("buy now")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    eventually(lambda: _request_from(target, conn.key_hex), what="the contact request")
    target.decline(conn.key_hex)
    declined = conn.expect("contact", state="declined")
    conn.send({"type": "ack", "id": declined["id"]})
    eventually(lambda: not _request_from(target, conn.key_hex), what="the request to go")
    assert _shown(target, conn.key_hex) == []
    conn.close()


def message_delivered_once(target: Target) -> None:
    """A contact's message is acked, shown once, and a duplicate is acked again but not shown."""
    conn = RawConn(target.address, name="friend").handshake()
    _become_contact(target, conn)
    m = _msg("Are you home? مرحبا")
    for _ in range(2):
        conn.send(m)
        conn.expect("ack", id=m["id"])
    shown = eventually(lambda: [x for x in _shown(target, conn.key_hex) if x["id"] == m["id"]],
                       what="the message")
    time.sleep(0.2)
    assert len([x for x in _shown(target, conn.key_hex) if x["id"] == m["id"]]) == 1, "shown twice"
    assert shown[0]["text"] == m["text"]
    conn.close()


def outbox_delivered_and_cleared(target: Target) -> None:
    """The target's message to a chat peer is delivered, and its outbox clears on the ack."""
    me = ChatPeer(name="reference").start()
    try:
        target.add_contact(me.address, name="reference")
        mid = target.send_text(me.address, "from the target")
        req = me.wait_for(lambda: me.requests.get(target.key))
        assert [m.text for m in req.held] == ["from the target"]
        assert [m.id for m in req.held] == [mid]
        eventually(lambda: target.waiting(me.address) == [], what="the target's outbox to clear")
        me.accept(target.key)
        contact = eventually(lambda: [c for c in target.contacts() if c["key"] == me.key_hex
                                      and c.get("accepted_by_them")], what="`contact accepted` to land")
        assert contact
    finally:
        me.stop()


def key_changed_refused(target: Target) -> None:
    """Another key under a contact's name is refused with `error key_changed`, then accepted again."""
    name = "laptop-" + new_id()[:6]
    old = RawConn(target.address, name=name).handshake()
    _become_contact(target, old)
    old.close()
    new = RawConn(target.address, name=name).open()
    new.hello()
    new.auth()
    try:
        new.expect("auth")
        err = new.expect("error", code="key_changed")
    except ws.Closed:
        raise AssertionError("closed without `error key_changed`")
    assert err["code"] == "key_changed"
    assert new.expect_closed() in (protocol.CLOSE_KEY_CHANGED, 1006)
    flagged = eventually(lambda: [c for c in target.contacts() if c.get("flagged_key") == new.key_hex],
                         what="the contact to be flagged")
    target.accept_new_key(flagged[0]["name"])
    again = RawConn(target.address, name=name, seed=new.seed).handshake()
    m = _msg("it is me, reinstalled")
    again.send(m)
    again.expect("ack", id=m["id"])
    eventually(lambda: [x for x in _shown(target, new.key_hex) if x["id"] == m["id"]],
               what="the message under the new key")
    again.close()


def voice_message_whole_and_broken(target: Target) -> None:
    """A voice message is acked only once whole; a broken one is not, and is sent again."""
    with open(os.path.join(VECTORS, "voice.json"), encoding="utf-8") as f:
        vector = json.load(f)
    conn = RawConn(target.address, name="talker").handshake()
    _become_contact(target, conn)
    voice = json.loads(vector["message"])
    voice["id"] = new_id()
    conn.send(voice)
    conn.send_binary(bytes.fromhex(vector["chunks"][0]["frame"]))
    early = [f for f in conn.drain(0.5) if isinstance(f, dict) and f.get("id") == voice["id"]]
    assert not early, f"a voice message was answered before it was whole: {early}"
    conn.close()
    conn = RawConn(target.address, name="talker", seed=conn.seed).handshake()
    conn.send(voice)
    for chunk in vector["chunks"]:
        conn.send_binary(bytes.fromhex(chunk["frame"]))
    conn.expect("ack", id=voice["id"])
    got = eventually(lambda: [m for m in target.messages(conn.key_hex) if m["id"] == voice["id"]],
                     what="the voice message")
    assert got[0].get("voice_bytes", vector["bytes"]) == vector["bytes"], got
    conn.close()


SCENARIOS = [handshake, bad_signature_refused, unknown_type_answered, invalid_frames_answered,
             contact_request_accepted, contact_request_declined, message_delivered_once,
             outbox_delivered_and_cleared, key_changed_refused, voice_message_whole_and_broken]
WIRE_ONLY = [handshake, bad_signature_refused, unknown_type_answered, invalid_frames_answered]


# -- messages: the outbox, order, receipts, typing, replies, reactions ----------
# (ticket 05; ticket 04 runs them against the simulator)

def _theirs(target: Target, key: str, message_id: str):
    return [m for m in target.messages(key) if m["id"] == message_id]


def outbox_across_disconnect(target: Target) -> None:
    """A message to a chat peer that is away waits in the outbox, and is delivered once it is back."""
    away = ChatPeer(name="away").start()
    address, seed = away.address, away.seed
    away.stop()
    back = None
    try:
        target.add_contact(address, name="away")
        mid = target.send_text(address, "while you were away")
        time.sleep(0.5)
        assert target.waiting(address) == [mid], target.waiting(address)
        back = ChatPeer(name="away", seed=seed, port=address[1]).start()
        target.flush(address)
        req = back.wait_for(lambda: back.requests.get(target.key))
        assert [m.id for m in req.held] == [mid], req.held
        eventually(lambda: target.waiting(address) == [], what="the target's outbox to clear")
        mine = [m for m in target.messages(address) if m["id"] == mid]
        assert mine and mine[0]["state"] == "delivered", mine
    finally:
        if back:
            back.stop()


def messages_in_order_written(target: Target) -> None:
    """A conversation is ordered by when each message was written, not by arrival."""
    conn = RawConn(target.address, name="late").handshake()
    _become_contact(target, conn)
    late, early = _msg("written second"), _msg("written first")
    early["ts"] = late["ts"] - 60_000
    for m in (late, early):
        conn.send(m)
        conn.expect("ack", id=m["id"])
    shown = eventually(lambda: len(_shown(target, conn.key_hex)) >= 3 and _shown(target, conn.key_hex),
                       what="both messages")
    texts = [m["text"] for m in shown if m["id"] in (late["id"], early["id"])]
    assert texts == ["written first", "written second"], texts
    conn.close()


def read_receipts_on_and_off(target: Target) -> None:
    """Reading sends `read` with the ids; with read receipts off, nothing goes."""
    conn = RawConn(target.address, name="reader").handshake()
    _become_contact(target, conn)
    m = _msg("did you read this?")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    eventually(lambda: _theirs(target, conn.key_hex, m["id"]), what="the message")
    target.mark_read(conn.key_hex)
    read = conn.expect("read")
    assert m["id"] in read["ids"], read
    target.set_read_receipts(False)
    try:
        quiet = _msg("and this one?")
        conn.send(quiet)
        conn.expect("ack", id=quiet["id"])
        eventually(lambda: _theirs(target, conn.key_hex, quiet["id"]), what="the second message")
        target.mark_read(conn.key_hex)
        sent = [f for f in conn.drain(0.7) if isinstance(f, dict) and f.get("type") == "read"]
        assert not sent, f"read receipts are off, yet the target sent {sent}"
        assert _theirs(target, conn.key_hex, quiet["id"])[0]["state"] == "read"
    finally:
        target.set_read_receipts(True)
    # Ours, read by the other side.
    mid = target.send_text(conn.key_hex, "read mine")
    conn.expect("msg", id=mid)
    conn.send({"type": "ack", "id": mid})
    conn.send({"type": "read", "ids": [mid]})
    eventually(lambda: [x for x in target.messages(conn.key_hex) if x["id"] == mid and x["state"] == "read"],
               what="our message to show as read")
    conn.close()


def typing_both_ways(target: Target) -> None:
    """`typing` shows and stops; the target's own is sent; a stranger's is ignored."""
    conn = RawConn(target.address, name="typist").handshake()
    _become_contact(target, conn)
    conn.send({"type": "typing", "on": True})
    eventually(lambda: target.is_typing(conn.key_hex), what="typing to show")
    conn.send({"type": "typing", "on": False})
    eventually(lambda: not target.is_typing(conn.key_hex), what="typing to stop")
    target.set_typing(conn.key_hex, True)
    conn.expect("typing", on=True)
    target.set_typing(conn.key_hex, False)
    conn.expect("typing", on=False)
    stranger = RawConn(target.address, name="nosy").handshake()
    stranger.send({"type": "typing", "on": True})
    time.sleep(0.3)
    assert not target.is_typing(stranger.key_hex), "a stranger's typing was shown"
    stranger.close()
    conn.close()


def replies_both_ways(target: Target) -> None:
    """A reply names what it answers, in both directions."""
    conn = RawConn(target.address, name="replier").handshake()
    _become_contact(target, conn)
    question = _msg("tea or coffee?")
    conn.send(question)
    conn.expect("ack", id=question["id"])
    eventually(lambda: _theirs(target, conn.key_hex, question["id"]), what="the question")
    answer = target.send_text(conn.key_hex, "tea", reply_to=question["id"])
    got = conn.expect("msg", id=answer)
    assert got.get("reply_to") == question["id"], got
    conn.send({"type": "ack", "id": answer})
    follow = _msg("milk?", reply_to=answer)
    conn.send(follow)
    conn.expect("ack", id=follow["id"])
    shown = eventually(lambda: _theirs(target, conn.key_hex, follow["id"]), what="the reply")
    assert shown[0].get("reply_to") == answer, shown
    conn.close()


def reactions_both_ways(target: Target) -> None:
    """A reaction is acked, shown once, replaced and removed; the target's own is sent."""
    conn = RawConn(target.address, name="reactor").handshake()
    _become_contact(target, conn)
    mid = target.send_text(conn.key_hex, "I passed!")
    conn.expect("msg", id=mid)
    conn.send({"type": "ack", "id": mid})

    def theirs():
        mine = [m for m in target.messages(conn.key_hex) if m["id"] == mid]
        return (mine[0].get("reactions") or {}).get("them") if mine else None

    thumbs = {"type": "react", "id": new_id(), "ts": now_ms(), "to": mid, "emoji": "👍"}
    for _ in range(2):  # a duplicate is acked again and applied once
        conn.send(thumbs)
        conn.expect("ack", id=thumbs["id"])
    eventually(lambda: theirs() == "👍", what="the reaction")
    party = dict(thumbs, id=new_id(), ts=thumbs["ts"] + 1000, emoji="🎉")
    conn.send(party)
    conn.expect("ack", id=party["id"])
    eventually(lambda: theirs() == "🎉", what="the reaction replaced")
    removed = dict(thumbs, id=new_id(), ts=thumbs["ts"] + 2000, emoji="")
    conn.send(removed)
    conn.expect("ack", id=removed["id"])
    eventually(lambda: theirs() is None, what="the reaction removed")

    m = _msg("well done")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    eventually(lambda: _theirs(target, conn.key_hex, m["id"]), what="the message")
    rid = target.react(conn.key_hex, m["id"], "❤️")
    react = conn.expect("react", id=rid)
    assert react["to"] == m["id"] and react["emoji"] == "❤️", react
    conn.send({"type": "ack", "id": rid})
    eventually(lambda: target.waiting(conn.key_hex) == [], what="the reaction to leave the outbox")
    conn.close()


MESSAGE_SCENARIOS = [outbox_across_disconnect, messages_in_order_written, read_receipts_on_and_off,
                     typing_both_ways, replies_both_ways, reactions_both_ways]
SCENARIOS += MESSAGE_SCENARIOS


# -- voice messages and calls ----------------------------------------------------
# (ticket 10; ticket 09 points them at the simulator.) The target's side of a
# call is driven through Target: place_call, calls, answer_call, end_call,
# talk, set_call_policy, cellular_call; the other side is a RawConn.

def _vector(name: str) -> dict:
    with open(os.path.join(VECTORS, name), encoding="utf-8") as f:
        return json.load(f)


def _voice_file() -> tuple[str, bytes]:
    path = os.path.join(VECTORS, "voice.opus")
    with open(path, "rb") as f:
        return path, f.read()


def _receive_voice(conn: RawConn, timeout: float = 5.0, stop_after_chunks: int | None = None):
    """The next `voice` frame and its chunks, in order: (frame, data). With
    `stop_after_chunks`, return as soon as that many have arrived."""
    voice = conn.expect("voice", timeout=timeout)
    assert voice.get("codec") == "ogg-opus", voice
    data, seq = b"", 0
    deadline = time.monotonic() + timeout
    while len(data) < voice["bytes"]:
        got = conn.recv(max(0.05, deadline - time.monotonic()))
        if not (isinstance(got, tuple) and got[0] == "binary"):
            continue
        kind, stream, s, payload = protocol.unpack_binary(got[1])
        if kind != protocol.KIND_VOICE or stream != voice["stream"]:
            continue
        assert s == seq, f"voice chunk {s} where {seq} was due"
        assert len(payload) <= protocol.MAX_CHUNK, len(payload)
        data += payload
        seq += 1
        if stop_after_chunks and seq >= stop_after_chunks:
            break
    return voice, data


def _call_audio(conn: RawConn, stream: int, count: int, timeout: float = 5.0) -> list:
    """`count` call audio frames from the target on `stream`: [(seq, payload)]."""
    out, deadline = [], time.monotonic() + timeout
    while len(out) < count:
        try:
            got = conn.recv(max(0.05, deadline - time.monotonic()))
        except AssertionError:
            raise AssertionError(f"{len(out)} of {count} call audio frames in {timeout} s")
        if isinstance(got, tuple) and got[0] == "binary":
            kind, s, seq, payload = protocol.unpack_binary(got[1])
            if kind == protocol.KIND_CALL_AUDIO:
                assert s == stream, f"call audio on stream {s}, not the call's {stream}"
                out.append((seq, payload))
        if time.monotonic() > deadline:
            raise AssertionError(f"{len(out)} of {count} call audio frames in {timeout} s")
    return out


def _speak(conn: RawConn, stream: int, frames: int, start_seq: int = 0) -> None:
    """Send the vector's Opus packets as call audio, 20 ms apart."""
    packets = [bytes.fromhex(f["hex"]) for f in _vector("voice.json")["frames"]]
    t0 = time.monotonic()
    for i in range(frames):
        conn.send_binary(protocol.pack_binary(protocol.KIND_CALL_AUDIO, stream, start_seq + i,
                                              packets[i % len(packets)]))
        time.sleep(max(0.0, t0 + (i + 1) * 0.02 - time.monotonic()))


def _call_state(target: Target, call: str):
    return (target.calls().get(call) or {}).get("state")


def _offer(conn: RawConn, target: Target) -> tuple[str, int]:
    """`conn` offers a call; the target rings. (call id, stream)."""
    call, stream = new_id(), int.from_bytes(os.urandom(4), "big") | 1
    conn.send({"type": "call_offer", "call": call, "transports": ["ws-opus"], "stream": stream})
    eventually(lambda: _call_state(target, call) == "ringing", what="the target to ring")
    return call, stream


def _connected_call(target: Target, name: str) -> tuple[RawConn, str, int]:
    """A contact calls the target, and its user answers."""
    target.set_call_policy("ignore")
    conn = RawConn(target.address, name=name).handshake()
    _become_contact(target, conn)
    call, stream = _offer(conn, target)
    target.answer_call(call)
    answer = conn.expect("call_answer", call=call)
    assert answer.get("transport") == "ws-opus" and "decline" not in answer, answer
    eventually(lambda: _call_state(target, call) == "connected", what="the call to connect")
    return conn, call, stream


def voice_message_from_target(target: Target) -> None:
    """The target's voice message goes as `voice` and chunks of the whole file; once acked it is delivered."""
    conn = RawConn(target.address, name="listener").handshake()
    _become_contact(target, conn)
    path, data = _voice_file()
    mid = target.send_voice(conn.key_hex, path, 1500)
    voice, got = _receive_voice(conn)
    assert voice["id"] == mid and voice["bytes"] == len(data) and voice["ms"] == 1500, voice
    assert got == data, "the voice message's bytes differ from the file sent"
    conn.send({"type": "ack", "id": mid})
    eventually(lambda: target.waiting(conn.key_hex) == [], what="the voice message to leave the outbox")
    eventually(lambda: [m for m in target.messages(conn.key_hex) if m["id"] == mid and m["state"] == "delivered"],
               what="the voice message to show as delivered")
    conn.close()


def voice_message_broken_and_restarted(target: Target) -> None:
    """A transfer broken by a closed connection is sent again, whole, on the next one."""
    conn = RawConn(target.address, name="flaky").handshake()
    _become_contact(target, conn)
    path, data = _voice_file()
    mid = target.send_voice(conn.key_hex, path, 1500)
    voice, partial = _receive_voice(conn, stop_after_chunks=1)
    assert voice["id"] == mid and len(partial) < len(data)
    conn.close()
    assert target.waiting(conn.key_hex) == [mid], "a voice message not acked left the outbox"
    again = RawConn(target.address, name="flaky", seed=conn.seed).handshake()
    target.flush(again.key_hex)
    voice2, whole = _receive_voice(again)
    assert voice2["id"] == mid and whole == data, "the voice message was not sent again whole"
    again.send({"type": "ack", "id": mid})
    eventually(lambda: target.waiting(again.key_hex) == [], what="the outbox to clear")
    again.close()


def call_answered(target: Target) -> None:
    """A contact's call rings; answered, audio flows both ways on the offer's stream; a hangup ends it."""
    conn, call, stream = _connected_call(target, "caller")
    target.talk(call)
    speaker = threading.Thread(target=_speak, args=(conn, stream, 25), daemon=True)
    speaker.start()
    audio = _call_audio(conn, stream, 10)
    speaker.join()
    seqs = [s for s, _ in audio]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs), f"call audio out of order: {seqs}"
    assert all(1 <= len(p) <= protocol.MAX_CHUNK for _, p in audio)
    eventually(lambda: (target.calls().get(call) or {}).get("frames", 0) >= 20, what="the target to receive audio")
    conn.send({"type": "call_end", "call": call, "reason": "hangup"})
    eventually(lambda: _call_state(target, call) == "ended:hangup", what="the call to end")
    conn.close()


def call_placed_by_target(target: Target) -> None:
    """The target offers ws-opus; answered, it sends audio on its stream; its hangup sends `call_end`."""
    conn = RawConn(target.address, name="callee").handshake()
    _become_contact(target, conn)
    call = target.place_call(conn.key_hex)
    offer = conn.expect("call_offer", call=call)
    assert "ws-opus" in offer["transports"], offer
    conn.send({"type": "call_answer", "call": call, "transport": "ws-opus"})
    eventually(lambda: _call_state(target, call) == "connected", what="the call to connect")
    target.talk(call)
    _call_audio(conn, offer["stream"], 10)
    _speak(conn, offer["stream"], 10)
    target.end_call(call)
    end = conn.expect("call_end", call=call)
    assert end["reason"] == "hangup", end
    conn.close()


def call_declined(target: Target) -> None:
    """The user declines a ringing call; a call the other side declines shows as declined."""
    target.set_call_policy("ignore")
    conn = RawConn(target.address, name="pest").handshake()
    _become_contact(target, conn)
    call, _ = _offer(conn, target)
    target.answer_call(call, decline="declined")
    answer = conn.expect("call_answer", call=call)
    assert answer.get("decline") == "declined" and "transport" not in answer, answer
    mine = target.place_call(conn.key_hex)
    conn.expect("call_offer", call=mine)
    conn.send({"type": "call_answer", "call": mine, "decline": "declined"})
    eventually(lambda: _call_state(target, mine) == "declined:declined", what="the target's call to be declined")
    conn.close()


def call_busy(target: Target) -> None:
    """A second call while one is up gets `busy`; a stranger's call gets `declined`."""
    first, call, stream = _connected_call(target, "first")
    second = RawConn(target.address, name="second").handshake()
    _become_contact(target, second)
    other = new_id()
    second.send({"type": "call_offer", "call": other, "transports": ["ws-opus"], "stream": 7})
    answer = second.expect("call_answer", call=other)
    assert answer.get("decline") == "busy", answer
    assert _call_state(target, call) == "connected", "the call in progress was disturbed"
    stranger = RawConn(target.address, name="stranger").handshake()
    odd = new_id()
    stranger.send({"type": "call_offer", "call": odd, "transports": ["ws-opus"], "stream": 9})
    assert stranger.expect("call_answer", call=odd).get("decline") == "declined"
    first.send({"type": "call_end", "call": call, "reason": "hangup"})
    for c in (first, second, stranger):
        c.close()


def call_ended_by_cellular(target: Target) -> None:
    """A cellular call takes over: the tailnet call ends with `call_end` `cellular`."""
    if not target.can_cellular:
        raise Skipped("the target has no cellular calls")
    conn, call, stream = _connected_call(target, "interrupted")
    target.cellular_call()
    end = conn.expect("call_end", call=call)
    assert end["reason"] == "cellular", end
    eventually(lambda: _call_state(target, call) == "ended:cellular", what="the call to end")
    conn.close()


def call_far_end_silent(target: Target) -> None:
    """A connected call that hears nothing for 10 s is ended by the target, `call_end` `timeout`."""
    conn, call, stream = _connected_call(target, "silent")
    t0 = time.monotonic()
    end = conn.expect("call_end", call=call, timeout=15)
    waited = time.monotonic() - t0
    assert end["reason"] == "timeout", end
    assert 9.5 <= waited <= 13.5, f"ended after {waited:.1f} s, not 10"
    eventually(lambda: _call_state(target, call) == "ended:timeout", what="the call to end")
    conn.close()


def call_unreachable(target: Target) -> None:
    """A call to a contact that cannot be reached fails at once as `unreachable`, and is not queued."""
    away = ChatPeer(name="away").start()
    address = away.address
    away.stop()
    target.add_contact(address, name="away")
    t0 = time.monotonic()
    call = target.place_call(address)
    eventually(lambda: _call_state(target, call) == "ended:unreachable", timeout=8,
               what="the call to fail as unreachable")
    assert time.monotonic() - t0 < 8
    assert target.waiting(address) == [], "a call went into the outbox"


VOICE_SCENARIOS = [voice_message_from_target, voice_message_broken_and_restarted]


# -- WebRTC calls between video-capable chat peers (section 4.11) -------------------
# (tailnet-video ticket 03.) The target's WebRTC is played through the control
# protocol: what it would send (rtc, camera), what it takes (rtc_received)
# and its connection's state (media). The other side is a RawConn.

WEBRTC_CAPS = list(protocol.CAPS_V1) + [protocol.CAP_WEBRTC]


def _webrtc(name: str) -> dict:
    return next(json.loads(v["text"]) for v in _vector("webrtc.json")["valid"] if v["name"] == name)


def _candidate(index: int) -> dict:
    return {"candidate": _vector("webrtc.json")["candidates"][index]["candidate"], "sdpMid": "0", "sdpMLineIndex": 0}


TAILNET_CANDIDATE, LAN_CANDIDATE = 0, 2


def _webrtc_call(target: Target, name: str) -> tuple[RawConn, str, float]:
    """A video-capable contact calls the target over WebRTC, and its user
    answers: (conn, call id, when the answer came)."""
    target.set_call_policy("ignore")
    target.set_webrtc(True)
    conn = RawConn(target.address, name=name, caps=WEBRTC_CAPS).handshake()
    assert protocol.CAP_WEBRTC in conn.their["caps"], conn.their["caps"]
    _become_contact(target, conn)
    call = new_id()
    conn.send({"type": "call_offer", "call": call, "transports": ["webrtc", "ws-opus"], "stream": 5, "video": False})
    eventually(lambda: _call_state(target, call) == "ringing", what="the target to ring")
    target.answer_call(call)
    answer = conn.expect("call_answer", call=call)
    answered = time.monotonic()
    assert answer.get("transport") == "webrtc", answer
    eventually(lambda: _call_state(target, call) == "connected", what="the call to connect")
    return conn, call, answered


def webrtc_offered_by_capability(target: Target) -> None:
    """The target offers `webrtc` first, with `video`, only when both hellos
    announce call:webrtc; else `ws-opus` alone, with no `video`."""
    target.set_webrtc(True)
    for caps, want in ((WEBRTC_CAPS, ["webrtc", "ws-opus"]), (list(protocol.CAPS_V1), ["ws-opus"])):
        conn = RawConn(target.address, name="callee-" + want[0], caps=caps).handshake()
        _become_contact(target, conn)
        call = target.place_call(conn.key_hex, video=True)
        offer = conn.expect("call_offer", call=call)
        assert offer["transports"] == want, (caps, offer)
        if want[0] == "webrtc":
            assert offer.get("video") is True, offer
        else:
            assert "video" not in offer, offer
        target.end_call(call)
        conn.close()
    target.set_webrtc(False)
    conn = RawConn(target.address, name="callee-plain", caps=WEBRTC_CAPS).handshake()
    assert protocol.CAP_WEBRTC not in conn.their["caps"], conn.their["caps"]
    _become_contact(target, conn)
    call = target.place_call(conn.key_hex)
    assert conn.expect("call_offer", call=call)["transports"] == ["ws-opus"]
    target.end_call(call)
    conn.close()


def webrtc_unannounced_answers_ws_opus(target: Target) -> None:
    """A target without call:webrtc (the T-Deck) announces none, and answers
    an offer of both with `ws-opus`."""
    target.set_webrtc(False)
    target.set_call_policy("ignore")
    conn = RawConn(target.address, name="video-caller", caps=WEBRTC_CAPS).handshake()
    assert protocol.CAP_WEBRTC not in conn.their["caps"], conn.their["caps"]
    _become_contact(target, conn)
    call, stream = new_id(), 11
    conn.send({"type": "call_offer", "call": call, "transports": ["webrtc", "ws-opus"], "stream": stream, "video": True})
    eventually(lambda: _call_state(target, call) == "ringing", what="the target to ring")
    target.answer_call(call)
    answer = conn.expect("call_answer", call=call)
    assert answer.get("transport") == "ws-opus", answer
    conn.send({"type": "call_end", "call": call, "reason": "hangup"})
    eventually(lambda: _call_state(target, call) == "ended:hangup", what="the call to end")
    conn.close()


def webrtc_picked_by_target(target: Target) -> None:
    """A video-capable target picks `webrtc` whenever it is offered, first or not."""
    target.set_call_policy("ignore")
    target.set_webrtc(True)
    conn = RawConn(target.address, name="caller", caps=WEBRTC_CAPS).handshake()
    _become_contact(target, conn)
    call = new_id()
    conn.send({"type": "call_offer", "call": call, "transports": ["ws-opus", "webrtc"], "stream": 5, "video": True})
    eventually(lambda: _call_state(target, call) == "ringing", what="the target to ring")
    assert target.calls()[call].get("video") is True, target.calls()[call]
    target.answer_call(call)
    assert conn.expect("call_answer", call=call).get("transport") == "webrtc"
    conn.send({"type": "call_end", "call": call, "reason": "hangup"})
    conn.close()


def webrtc_signalling_relayed(target: Target) -> None:
    """SDP and candidates go both ways as `call_rtc`; a candidate outside the
    tailnet is neither sent nor taken, in a frame or inside an SDP."""
    conn, call, _ = _webrtc_call(target, "rtc")
    v = _vector("webrtc.json")
    conn.send({"type": "call_rtc", "call": call, "sdp": {"type": "offer", "sdp": v["sdp"]["written"]}})
    conn.send({"type": "call_rtc", "call": call, "candidate": _candidate(LAN_CANDIDATE)})
    conn.send({"type": "call_rtc", "call": call, "candidate": _candidate(TAILNET_CANDIDATE)})
    conn.send({"type": "call_rtc", "call": new_id(), "candidate": _candidate(TAILNET_CANDIDATE)})  # unknown call
    want = [{"sdp": {"type": "offer", "sdp": v["sdp"]["tailnet_only"]}}, {"candidate": _candidate(TAILNET_CANDIDATE)}]
    got = eventually(lambda: (lambda r: r if len(r) >= 2 else None)(target.rtc_received(call)),
                     what="the SDP and the candidate to reach the target's WebRTC")
    time.sleep(0.3)
    assert target.rtc_received(call) == want, target.rtc_received(call)
    assert got[:2] == want
    assert target.rtc(call, sdp={"type": "answer", "sdp": v["sdp"]["written"]}) is True
    sdp = conn.expect("call_rtc", call=call)
    assert sdp.get("sdp") == {"type": "answer", "sdp": v["sdp"]["tailnet_only"]}, sdp
    assert target.rtc(call, candidate=_candidate(LAN_CANDIDATE)) is False, "a LAN candidate was sent"
    assert target.rtc(call, candidate=_candidate(TAILNET_CANDIDATE)) is True
    cand = conn.expect("call_rtc", call=call)
    assert cand.get("candidate") == _candidate(TAILNET_CANDIDATE), cand
    assert not [f for f in conn.drain(0.5) if isinstance(f, dict) and f["type"] in ("call_rtc", "error")]
    conn.send({"type": "call_end", "call": call, "reason": "hangup"})
    conn.close()


def webrtc_camera_states(target: Target) -> None:
    """`call_camera` both ways: the target's goes out, and the other side's
    on, off, paused and none are what the target shows."""
    conn, call, _ = _webrtc_call(target, "camera")
    assert target.camera(call, "off") is True
    assert conn.expect("call_camera", call=call)["camera"] == "off"
    for state in protocol.CAMERA_STATES:
        conn.send({"type": "call_camera", "call": call, "camera": state})
        eventually(lambda: target.calls()[call].get("camera") == state, what=f"the camera shown as {state}")
    conn.send({"type": "call_camera", "call": new_id(), "camera": "on"})  # unknown call: ignored
    assert not [f for f in conn.drain(0.5) if isinstance(f, dict) and f["type"] == "error"]
    assert target.calls()[call]["camera"] == "none"
    conn.send({"type": "call_end", "call": call, "reason": "hangup"})
    conn.close()


def webrtc_never_connected(target: Target) -> None:
    """A WebRTC call whose media is not connected 15 s after the answer is
    ended by the target with `call_end` `timeout`."""
    conn, call, answered = _webrtc_call(target, "unconnected")
    end = conn.expect("call_end", call=call, timeout=20)
    waited = time.monotonic() - answered
    assert end["reason"] == "timeout", end
    assert 14.5 <= waited <= 18.5, f"ended after {waited:.1f} s, not 15"
    eventually(lambda: _call_state(target, call) == "ended:timeout", what="the call to end")
    conn.close()


def webrtc_disconnected(target: Target) -> None:
    """Connected, a WebRTC call sends no frames of ours and is not ended for
    silence; disconnected for 10 s, it is ended with `call_end` `error`."""
    conn, call, _ = _webrtc_call(target, "dropped")
    target.media(call, "connected")
    quiet = [f for f in conn.drain(11.5) if isinstance(f, dict) and f["type"] == "call_end"]
    assert not quiet, f"a connected WebRTC call was ended: {quiet}"
    target.media(call, "disconnected")
    t0 = time.monotonic()
    end = conn.expect("call_end", call=call, timeout=15)
    waited = time.monotonic() - t0
    assert end["reason"] == "error", end
    assert 9.5 <= waited <= 13.5, f"ended after {waited:.1f} s, not 10"
    eventually(lambda: _call_state(target, call) == "ended:error", what="the call to end")
    conn.close()


def webrtc_busy(target: Target) -> None:
    """Another call during a WebRTC call gets `busy`; the WebRTC call goes on."""
    first, call, _ = _webrtc_call(target, "first")
    target.media(call, "connected")
    second = RawConn(target.address, name="second", caps=WEBRTC_CAPS).handshake()
    _become_contact(target, second)
    other = new_id()
    second.send({"type": "call_offer", "call": other, "transports": ["webrtc", "ws-opus"], "stream": 7, "video": True})
    assert second.expect("call_answer", call=other).get("decline") == "busy"
    assert _call_state(target, call) == "connected", "the WebRTC call was disturbed"
    first.send({"type": "call_end", "call": call, "reason": "hangup"})
    for c in (first, second):
        c.close()


WEBRTC_SCENARIOS = [webrtc_offered_by_capability, webrtc_unannounced_answers_ws_opus, webrtc_picked_by_target,
                    webrtc_signalling_relayed, webrtc_camera_states, webrtc_never_connected, webrtc_disconnected,
                    webrtc_busy]


# -- sharing the tailnet's peer list (ticket 14) ---------------------------------

PEERS = [{"name": "homelab", "addrs": ["100.64.0.2"]},
         {"name": "laptop", "addrs": ["100.64.0.3", "fd7a:115c:a1e0::3"]}]


def _within_limits(shared: list) -> None:
    assert len(shared) <= protocol.MAX_PEERS, f"{len(shared)} peers shared"
    for p in shared:
        assert 0 < len(p["name"].encode("utf-8")) <= protocol.MAX_NAME_BYTES, p
        assert 1 <= len(p["addrs"]) <= protocol.MAX_PEER_ADDRS, p
        assert all(protocol.in_tailnet(a) for a in p["addrs"]), f"an address outside the tailnet: {p}"


def _ask(conn: RawConn) -> list:
    conn.send(protocol.peers_ask())
    return conn.expect("peers")["peers"]


def peers_shared_with_a_contact(target: Target) -> None:
    """A contact's `peers_ask` is answered with the tailnet's online peers."""
    expected = target.share_peers(PEERS)
    conn = RawConn(target.address, name="asker").handshake()
    assert "peers" in conn.their["caps"], conn.their["caps"]
    _become_contact(target, conn)
    shared = _ask(conn)
    assert shared == expected, (shared, expected)
    _within_limits(shared)
    conn.close()


def peers_not_shared_with_a_stranger(target: Target) -> None:
    """A stranger's `peers_ask` is answered with an empty list."""
    target.share_peers(PEERS)
    conn = RawConn(target.address, name="nosy").handshake()
    assert _ask(conn) == []
    conn.close()


def peers_unannounced_is_unsupported(target: Target) -> None:
    """`peers_ask` from a peer whose hello did not announce `peers` draws
    `error unsupported`; an unasked `peers` is ignored."""
    target.share_peers(PEERS)
    quiet = RawConn(target.address, name="quiet", caps=[c for c in protocol.CAPS_V1 if c != "peers"]).handshake()
    _become_contact(target, quiet)
    quiet.send(protocol.peers_ask())
    err = quiet.expect("error", code="unsupported")
    assert err.get("what") == "peers_ask", err
    quiet.close()
    conn = RawConn(target.address, name="chatty").handshake()
    conn.send(protocol.peers(PEERS))
    m = _msg("still here")
    conn.send(m)
    conn.expect("ack", id=m["id"])
    conn.close()


def peers_kept_to_the_limits(target: Target) -> None:
    """At most 64 peers, two addresses each inside the tailnet, names of 64 bytes."""
    many = [{"name": "peer-%02d" % i, "addrs": ["100.64.1.%d" % i]} for i in range(70)]
    many.insert(0, {"name": "n" * 80, "addrs": ["192.168.1.2", "100.64.2.1", "fd7a:115c:a1e0::21", "100.64.2.2"]})
    many.insert(1, {"name": "lan-only", "addrs": ["192.168.1.3"]})
    expected = target.share_peers(many)
    conn = RawConn(target.address, name="counter").handshake()
    _become_contact(target, conn)
    shared = _ask(conn)
    assert shared == expected, (shared, expected)
    _within_limits(shared)
    conn.close()


def peers_asked_by_target(target: Target) -> None:
    """The target asks a contact for its peer list and gets it."""
    me = ChatPeer(name="sharer").start()
    try:
        me.tailnet_peers = PEERS
        me.add_contact(key=target.key, name="target")
        target.add_contact(me.address, name="sharer")
        got = target.ask_peers(me.address)
        assert got == protocol.shared_peers(PEERS, "127.0.0.1"), got
    finally:
        me.stop()


PEERS_SCENARIOS = [peers_shared_with_a_contact, peers_not_shared_with_a_stranger,
                   peers_unannounced_is_unsupported, peers_kept_to_the_limits, peers_asked_by_target]
CALL_SCENARIOS = [call_answered, call_placed_by_target, call_declined, call_busy, call_ended_by_cellular,
                  call_far_end_silent, call_unreachable]
SCENARIOS += VOICE_SCENARIOS + CALL_SCENARIOS + PEERS_SCENARIOS + WEBRTC_SCENARIOS


def run(make_target, scenarios=None, out=None, skipped: list | None = None) -> int:
    """Run each scenario against a fresh target; the number that failed.
    A scenario the target cannot do is reported as skipped, not failed, and
    its name added to `skipped`."""
    out = out or sys.stdout
    skipped = skipped if skipped is not None else []
    failed = 0
    for scenario in scenarios or SCENARIOS:
        target = make_target()
        try:
            scenario(target)
            print(f"ok    {scenario.__name__}", file=out)
        except Skipped as e:
            skipped.append(scenario.__name__)
            print(f"skip  {scenario.__name__}: {e}", file=out)
        except (AssertionError, Refused, ws.Closed, OSError, RuntimeError):
            failed += 1
            print(f"FAIL  {scenario.__name__}\n{traceback.format_exc()}", file=out)
        finally:
            target.stop()
    return failed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    how = ap.add_mutually_exclusive_group(required=True)
    how.add_argument("--reference", action="store_true", help="a reference chat peer in this process")
    how.add_argument("--control", help="a command that starts a chat peer speaking the control protocol")
    how.add_argument("--address", help="HOST:PORT of a running chat peer: the wire-only scenarios")
    ap.add_argument("--key", help="with --address: the target's identity key, hex")
    ap.add_argument("--only", action="append", help="run only this scenario (repeatable)")
    args = ap.parse_args(argv)
    if args.reference:
        make, pool = ReferenceTarget, SCENARIOS
    elif args.control:
        make, pool = (lambda: ProcessTarget(shlex.split(args.control))), SCENARIOS
    else:
        if not args.key:
            ap.error("--address needs --key")
        host, _, port = args.address.rpartition(":")
        make, pool = (lambda: AddressTarget((host.strip("[]"), int(port)), args.key)), WIRE_ONLY
    chosen = [s for s in pool if not args.only or s.__name__ in args.only]
    skipped: list = []
    failed = run(make, chosen, skipped=skipped)
    print(f"{len(chosen) - failed - len(skipped)} of {len(chosen)} scenarios passed"
          + (f", {len(skipped)} skipped" if skipped else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
