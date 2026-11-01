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

    def send_text(self, to, text: str) -> str:
        return self.call({"cmd": "send_text", "to": list(to) if isinstance(to, tuple) else to, "text": text})

    def waiting(self, who) -> list:
        return self.call({"cmd": "waiting", "to": list(who) if isinstance(who, tuple) else who})

    def stop(self):
        pass


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


def run(make_target, scenarios=None, out=None) -> int:
    """Run each scenario against a fresh target; the number that failed."""
    out = out or sys.stdout
    failed = 0
    for scenario in scenarios or SCENARIOS:
        target = make_target()
        try:
            scenario(target)
            print(f"ok    {scenario.__name__}", file=out)
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
    failed = run(make, chosen)
    print(f"{len(chosen) - failed} of {len(chosen)} scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
