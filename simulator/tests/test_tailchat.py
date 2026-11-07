#!/usr/bin/env python3
"""Tailnet chat on the simulated T-Deck Max: the kernel module against the
reference chat peer's conformance suite, its card and its file, and the
Chat app's screens.

The simulator runs the real kernel module (tailnet_chat.rs) on a scratch
copy of simulator/sdcard, with its pretend tailnet joined and on. It listens
on 127.0.0.1, port THISTLE_SIM_CHAT_PORT, and admits loopback, as only the
simulator may. THISTLE_SIM_CHAT_CONTROL gives the conformance suite a
control port for what only the device's user can do and see (accept,
decline, the contacts, the requests, a conversation); it calls the same
functions the app's C API does.

- conformance: tools/tailnet_chat/conformance.py's handshake, bad
  signature, unknown and invalid frames, contact request accepted and
  declined, and key changed scenarios, against one running simulator;
- the card: a held message under messages/tailnet/requests/, moved into
  the contact's file on Accept, deleted on Decline; a wrong path refused;
- a restart: the contacts and the flag on an impostor's key are kept in
  the module's own file, and a request waiting on the card is read back;
- the app: the Requests screen shows a request read back from the card,
  Accept from it makes the sender a contact, and the app says why chat is
  unavailable when Tailscale is off;
- discovery: Look for chat peers among the pretend tailnet's peers
  (THISTLE_SIM_TAILNET_PEERS=NAME=IP,...), two reference chat peers on
  127.0.0.2 and 127.0.0.3, and Add from its list.

Coordinates are the T-Deck Max's (240 x 320); taps start 2.25 s after
startup and come 1.5 s apart.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path
from typing import Callable, Optional

SIM_DIR = Path(__file__).resolve().parent.parent
REPO = SIM_DIR.parent
sys.path.insert(0, str(REPO / "tools"))
from tailnet_chat import conformance, ed25519, protocol  # noqa: E402
from tailnet_chat.conformance import RawConn, eventually  # noqa: E402
from tailnet_chat.peer import ChatPeer  # noqa: E402

APP = "com.thistle.tailchat"
# Every scenario but the calls, which have a case of their own.
SCENARIOS = [s for s in conformance.SCENARIOS
             if s not in conformance.CALL_SCENARIOS and s not in conformance.WEBRTC_SCENARIOS]
# The device is not video-capable: of the WebRTC scenarios, the one that
# shows it announces no call:webrtc and answers an offer of both with ws-opus.
CALLS = conformance.CALL_SCENARIOS + [conformance.webrtc_unannounced_answers_ws_opus]

# The app's tabs, along the bottom, and a tab's first row, on the T-Deck Max.
TAB_REQUESTS = "200,298"
TAB_CONTACTS = "120,298"
FIRST_ROW = "120,91"
# On a request's page, the right of the two buttons along the bottom.
ACCEPT = "180,300"
# Contacts' second row, then the add page's button.
ADD_CONTACT = "120,143"
ADD = "120,290"
# Settings, at a tab's top right, and its second row.
SETTINGS = "218,44"
CHECK_INTERVAL = "120,230"
# The first contact, Contacts' third row; on its page, Message.
CONTACT_ROW = "120,195"
MESSAGE = "82,205"
# Contacts' first row, Look for chat peers; on the page it opens, the first
# chat peer found.
LOOK = "120,91"
FIRST_FOUND = "120,127"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Device:
    """A simulator on a scratch card and SPIFFS, which a restart keeps."""

    def __init__(self, simulator: Path, root: Path, name: str, tailnet: bool = True, history: bool = True):
        self.simulator = simulator
        self.card = root / name
        self.spiffs = root / f"{name}-spiffs"
        shutil.copytree(SIM_DIR / "sdcard", self.card)
        self.spiffs.mkdir()
        (self.spiffs / ".tailnet_joined").write_text("1")
        system_path = self.card / "config" / "system.json"
        system = json.loads(system_path.read_text())
        system.get("thistle_os", system)["tailnet"] = {"enabled": tailnet}
        if not history:
            system.get("thistle_os", system).setdefault("messenger", {})["save_history"] = False
        system_path.write_text(json.dumps(system, indent=2))
        self.proc: Optional[subprocess.Popen] = None
        self.log_path = root / f"{name}.log"

    def env(self, **extra) -> dict:
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(self.card), THISTLE_SIM_SPIFFS=str(self.spiffs))
        for name in ("THISTLE_SIM_TAILNET", "THISTLE_SIM_TAILNET_PEERS", "THISTLE_SIM_TAILNET_LOGIN_S"):
            env.pop(name, None)
        env.update({k: str(v) for k, v in extra.items()})
        return env

    def start(self, **env) -> "Device":
        self.port, self.control_port = free_port(), free_port()
        self.log = open(self.log_path, "a")
        self.proc = subprocess.Popen(
            [str(self.simulator), "--headless", "--device", "tdeck-max", "--timeout", "600000"],
            env=self.env(THISTLE_SIM_CHAT_PORT=self.port, THISTLE_SIM_CHAT_CONTROL=self.control_port, **env),
            stdout=self.log, stderr=subprocess.STDOUT)
        return self

    def stop(self) -> None:
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(5)
            self.proc = None
            self.log.close()

    def log_text(self) -> str:
        return self.log_path.read_text(errors="replace")

    def run(self, args: list[str], timeout_ms: int, screenshot: Optional[Path] = None, **env) -> str:
        """A run of its own with taps, to its end: its log."""
        command = [str(self.simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(timeout_ms), *args]
        if screenshot:
            command += ["--screenshot", str(screenshot)]
        result = subprocess.run(command, env=self.env(**{"THISTLE_SIM_CHAT_PORT": free_port(), **env}), text=True,
                                capture_output=True, check=False, timeout=90)
        log = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(f"simulator exited {result.returncode}: {log.strip()[-400:]!r}")
        return log

    def chat_file(self, key: str, request: bool = False) -> Path:
        fp = protocol.fingerprint_id(bytes.fromhex(key))
        sub = "tailnet/requests" if request else "tailnet"
        return self.card / "messages" / sub / f"{fp}.jsonl"


class SimTarget(conformance.Target):
    """The simulator, as the conformance suite drives a target: its address
    and key from the control port's ready line, and the user's actions
    through it."""

    def __init__(self, device: Device, timeout: float = 30.0):
        self.device = device
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", device.control_port), timeout=2)
                break
            except OSError:
                if time.monotonic() > deadline or device.proc.poll() is not None:
                    raise RuntimeError("the simulator's control port did not open:\n" + device.log_text()[-2000:])
                time.sleep(0.2)
        self.sock.settimeout(timeout)
        self.reader = self.sock.makefile("r", encoding="utf-8")
        ready = json.loads(self.reader.readline())["ready"]
        self.key = ready["key"]
        self.address = tuple(ready["address"])
        self.seq = 0
        self.lock = threading.Lock()

    def call(self, command: dict):
        with self.lock:
            self.seq += 1
            self.sock.sendall((json.dumps(dict(command, seq=self.seq)) + "\n").encode())
            while True:
                reply = json.loads(self.reader.readline())
                if reply.get("seq") == self.seq:
                    break
        if "error" in reply:
            raise RuntimeError(f"{command['cmd']}: {reply['error']}")
        return reply["ok"]

    def logged(self, text: str) -> bool:
        """Whether the log says `text` yet: a command first, which flushes
        what the UI has printed."""
        self.call({"cmd": "unread"})
        return text in self.device.log_text()

    def close(self) -> None:
        self.sock.close()

    # The calls: what only the device's user does goes through the call
    # screen's own functions (rs_call_answer, rs_call_hangup); the cellular
    # call is the fake modem's --call, rung at once.

    def set_call_policy(self, policy: str):
        assert policy == "ignore", "the simulator's calls ring for its user"
        return True

    def talk(self, call: str):
        return True   # its microphone (THISTLE_SIM_MIC) streams by itself

    def set_webrtc(self, on: bool):
        if on:
            raise conformance.Skipped("the T-Deck is not video-capable")
        return True   # it never announces call:webrtc

    def screenshot(self, path: Path) -> None:
        self.call({"cmd": "screenshot", "file": str(path)})


def msg(text: str) -> dict:
    return {"type": "msg", "id": os.urandom(16).hex(), "ts": int(time.time() * 1000), "text": text}


# -- the cases -----------------------------------------------------------------

def case_conformance(simulator: Path, root: Path) -> None:
    dev = Device(simulator, root, "conformance").start()
    try:
        target = SimTarget(dev)
        out = []

        class Out:
            def write(self, s):
                out.append(s)

        failed = conformance.run(lambda: target, SCENARIOS, out=Out())
        report = "".join(out)
        print("      " + report.strip().replace("\n", "\n      "))
        assert failed == 0, f"{failed} scenarios failed"
        log = dev.log_text()
        for line in ("[tailnet_chat] listening on 127.0.0.1:", "a stranger", "contact request from stranger",
                     "refused 127.0.0.1: a signature that does not verify",
                     "not the key pinned for laptop-",
                     # The pretend tailnet's two online peers, homelab and laptop.
                     "told asker 2 tailnet peers", "told nosy 0 tailnet peers"):
            assert line in log, f"no {line!r} in the log"
        # The voice message received whole is kept beside the history, and
        # nothing of the broken transfer is.
        voice = (VECTORS / "voice.opus").read_bytes()
        kept = list((dev.card / "messages" / "tailnet" / "voice").iterdir())
        assert any(p.suffix == ".opus" and p.read_bytes() == voice for p in kept), kept
        assert not [p for p in kept if p.suffix != ".opus"], kept
        target.close()
    finally:
        dev.stop()


def case_card_and_restart(simulator: Path, root: Path) -> None:
    dev = Device(simulator, root, "card").start()
    try:
        target = SimTarget(dev)
        # A wrong path is refused before the WebSocket.
        with socket.create_connection(target.address, timeout=5) as s:
            s.sendall(b"GET /chat HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
            assert s.recv(100).startswith(b"HTTP/1.1 404"), "a wrong path was not refused"

        friend = RawConn(target.address, name="friend").handshake()
        m = msg("held until accepted")
        friend.send(m)
        friend.expect("ack", id=m["id"])
        held = eventually(lambda: dev.chat_file(friend.key_hex, request=True).is_file() and
                          dev.chat_file(friend.key_hex, request=True).read_text(), what="the request's file")
        line = json.loads(held.splitlines()[0])
        assert line["from"] == friend.key_hex and line["text"] == m["text"] and line["dir"] == "in", line
        assert line["id"] == m["id"] and line["name"] == "friend", line
        target.accept(friend.key_hex)
        friend.expect("contact", state="accepted")
        assert not dev.chat_file(friend.key_hex, request=True).exists(), "the request's file stayed"
        kept = [json.loads(x) for x in dev.chat_file(friend.key_hex).read_text().splitlines()]
        assert [x["text"] for x in kept] == [m["text"]], kept
        friend.close()

        spam = RawConn(target.address, name="spam").handshake()
        s = msg("buy now")
        spam.send(s)
        spam.expect("ack", id=s["id"])
        eventually(lambda: dev.chat_file(spam.key_hex, request=True).is_file(), what="the spam's request file")
        target.decline(spam.key_hex)
        spam.expect("contact", state="declined")
        assert not dev.chat_file(spam.key_hex, request=True).exists(), "a declined request's file stayed"
        assert not dev.chat_file(spam.key_hex).exists()
        spam.close()

        # A stranger waiting, an impostor flagged, then a restart.
        waiting = RawConn(target.address, name="waiting").handshake()
        w = msg("are you there?")
        waiting.send(w)
        waiting.expect("ack", id=w["id"])
        waiting.close()
        impostor = RawConn(target.address, name="friend").open()
        impostor.hello()
        impostor.auth()
        impostor.expect("auth")
        impostor.expect("error", code="key_changed")
        eventually(lambda: any(c.get("flagged_key") == impostor.key_hex for c in target.contacts()),
                   what="the flag")
        target.close()
        dev.stop()

        saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
        assert [c["key"] for c in saved["contacts"]] == [friend.key_hex], saved

        dev.start()
        target = SimTarget(dev)
        contacts = target.contacts()
        assert [(c["key"], c["name"], c["flagged_key"]) for c in contacts] == \
            [(friend.key_hex, "friend", impostor.key_hex)], contacts
        assert [r["key"] for r in target.requests()] == [waiting.key_hex], target.requests()
        assert [x["text"] for x in target.requests()[0]["held"]] == ["are you there?"]
        shown = target.messages(friend.key_hex)
        assert [x["text"] for x in shown] == [m["text"]], shown
        # Still a contact after the restart: its messages are taken.
        again = RawConn(target.address, name="friend", seed=friend.seed).handshake()
        m2 = msg("after the restart")
        again.send(m2)
        again.expect("ack", id=m2["id"])
        eventually(lambda: [x for x in target.messages(friend.key_hex) if x["id"] == m2["id"]],
                   what="the message after the restart")
        again.close()
        target.call({"cmd": "rename", "contact": friend.key_hex, "name": "Fred"})
        target.call({"cmd": "remove", "contact": "Fred"})
        assert target.contacts() == []
        target.close()
    finally:
        dev.stop()


def case_messages(simulator: Path, root: Path) -> None:
    """Alerts, the status bar's count and Home's row; receipts, a reply and
    reactions on the card; the conversation on screen; then a restart: the
    outbox, the history and the ids seen are kept."""
    dev = Device(simulator, root, "messages").start()
    try:
        target = SimTarget(dev)
        pal = RawConn(target.address, name="pal").handshake()
        conformance._become_contact(target, pal)
        m = msg("are you there?")
        pal.send(m)
        pal.expect("ack", id=m["id"])
        eventually(lambda: target.logged("(statusbar) unread 1"), what="the status bar's count")
        eventually(lambda: target.logged("now: 1 new message (Chat)"), what="Home's row", timeout=8)
        eventually(lambda: target.logged("pal: are you there?"), what="the pop-up")
        assert "[sim_haptic] message buzz" in dev.log_text(), "no buzz"
        assert [c["unread"] for c in target.contacts() if c["key"] == pal.key_hex] == [1]

        # A reply, read by pal; a reaction each way.
        mid = target.send_text(pal.key_hex, "yes, here", reply_to=m["id"])
        got = pal.expect("msg", id=mid)
        assert got["reply_to"] == m["id"], got
        pal.send({"type": "ack", "id": mid})
        pal.send({"type": "read", "ids": [mid]})
        rid = target.react(pal.key_hex, m["id"], "👍")
        pal.expect("react", id=rid)
        pal.send({"type": "ack", "id": rid})
        theirs = {"type": "react", "id": os.urandom(16).hex(), "ts": int(time.time() * 1000), "to": mid,
                  "emoji": "🎉"}
        pal.send(theirs)
        pal.expect("ack", id=theirs["id"])
        eventually(lambda: [x for x in target.messages(pal.key_hex) if x["id"] == mid and x["state"] == "read"
                            and x.get("reactions", {}).get("them") == "🎉"], what="our reply read and reacted to")

        # The conversation on screen: what was unread is read, and a new
        # message is read at once, with no alert.
        target.call({"cmd": "viewing", "to": pal.key_hex})
        assert m["id"] in pal.expect("read")["ids"]
        eventually(lambda: target.logged("(statusbar) unread 0"), what="the count to clear")
        seen = msg("seen at once")
        pal.send(seen)
        pal.expect("ack", id=seen["id"])
        assert pal.expect("read")["ids"] == [seen["id"]]
        time.sleep(0.6)
        assert not target.logged("pal: seen at once"), "alerted for the conversation on screen"
        assert target.call({"cmd": "unread"}) == 0
        target.call({"cmd": "viewing", "to": None})

        # Everything on the card, in the order it happened.
        lines = [json.loads(x) for x in dev.chat_file(pal.key_hex).read_text().splitlines()]
        events = [(x["dir"], x.get("text"), x.get("state"), x.get("emoji")) for x in lines]
        for e in [("in", m["text"], "received", None), ("out", "yes, here", "waiting", None),
                  ("out", None, "delivered", None), ("out", None, "read", None), ("out", None, None, "👍"),
                  ("in", None, None, "🎉"), ("in", None, "read", None)]:
            assert e in events, f"no {e} in {events}"
        assert [x for x in lines if x.get("id") == mid and "text" in x][0]["reply_to"] == m["id"]

        # Someone away: what is written to it waits, across a restart.
        away = ChatPeer(name="away").start()
        address, seed = away.address, away.seed
        away.stop()
        target.add_contact(address, name="away")
        wid = target.send_text(address, "kept across a restart")
        eventually(lambda: "could not reach away" in dev.log_text(), what="the dial to fail")
        pal.close()
        target.close()
        dev.stop()
        saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
        assert [f["id"] for b in saved["outbox"] for f in b["frames"]] == [wid], saved["outbox"]

        dev.start()
        target = SimTarget(dev)
        assert target.waiting(list(address)) == [wid]
        shown = {x["id"]: x for x in target.messages(pal.key_hex)}
        assert shown[mid]["state"] == "read" and shown[mid]["reply_to"] == m["id"], shown[mid]
        assert shown[mid]["reactions"] == {"them": "🎉"}, shown[mid]
        assert shown[m["id"]]["reactions"] == {"me": "👍"} and shown[m["id"]]["state"] == "read"
        # The same message again after the restart: acked, never shown twice.
        again = RawConn(target.address, name="pal", seed=pal.seed).handshake()
        again.send(m)
        again.expect("ack", id=m["id"])
        again.send(theirs)
        again.expect("ack", id=theirs["id"])
        time.sleep(0.3)
        assert [x["id"] for x in target.messages(pal.key_hex)].count(m["id"]) == 1, "shown twice"
        assert target.call({"cmd": "unread"}) == 0
        again.close()
        # The one away comes back and is seen online: what waits goes.
        back = ChatPeer(name="away", seed=seed, port=address[1]).start()
        try:
            target.flush(list(address))
            req = back.wait_for(lambda: back.requests.get(target.key), timeout=10)
            assert [x.id for x in req.held] == [wid], req.held
            eventually(lambda: target.waiting(list(address)) == [], what="the outbox to clear")
            assert any(c["key"] == back.key_hex for c in target.contacts()), "not pinned"
        finally:
            back.stop()
        log = dev.log_text()
        for line in ("dialling away at 127.0.0.1:", f"pinned away as {protocol.fingerprint(bytes.fromhex(back.key_hex))}",
                     f"delivered {wid} to away", "received " + m["id"] + " from pal again"):
            assert line in log, f"no {line!r} in the log"
        target.close()
    finally:
        dev.stop()


def case_sleep_check(simulator: Path, root: Path) -> None:
    """Ticket 07: the background check "tailnet chat", every 12 s here,
    dials a contact the peer list shows online with nothing waiting for it,
    and collects what waited on the other side: a laptop that cannot dial
    the device (it knows no address for it) still gets its message
    through. A contact added while online is reached at once and pinned."""
    dev = Device(simulator, root, "sleep")
    dev.start(THISTLE_SIM_TAILNET_LOOPBACK_PEER="laptop", THISTLE_SIM_CHAT_CHECK_S=12)
    laptop = ChatPeer(name="laptop").start()
    try:
        target = SimTarget(dev)
        eventually(lambda: target.logged("check tailnet chat registered: every 12 s"), what="the check registered")

        # Added while its peer is online: dialled at once, its key pinned.
        target.add_contact(laptop.address, name="laptop")
        eventually(lambda: any(c["key"] == laptop.key_hex for c in target.contacts()), what="the key pinned")
        assert "added: dialling laptop" in dev.log_text()
        # The sweep's connection closes once nothing more comes.
        eventually(lambda: not any(c["connected"] for c in target.contacts()), what="the sweep to close",
                   timeout=10)

        # Become each other's contact, then part.
        target.send_text(laptop.key_hex, "hello")
        laptop.wait_for(lambda: laptop.requests.get(target.key), timeout=10)
        laptop.accept(target.key)
        eventually(lambda: [c for c in target.contacts() if c["accepted_by_them"]], what="the answer")
        eventually(lambda: target.waiting(laptop.key_hex) == [], what="the device's outbox to clear")
        for conn in list(laptop.connections):
            conn.close()
        eventually(lambda: not any(c["connected"] for c in target.contacts()), what="the connection to close")

        # The laptop writes while the device is "asleep": it has no address
        # to dial, so the message waits there until the device's check.
        checks_before = dev.log_text().count("the background check: dialling laptop")
        mid = laptop.send_text(target.key, "while you slept")
        time.sleep(0.5)
        assert laptop.waiting(target.key) == [mid], laptop.waiting(target.key)
        eventually(lambda: laptop.delivered(mid), what="the check to collect it", timeout=30)
        assert dev.log_text().count("the background check: dialling laptop") > checks_before, \
            "delivered, but not by the check"
        eventually(lambda: [x for x in target.messages(laptop.key_hex) if x["id"] == mid],
                   what="the message shown")
        eventually(lambda: target.logged("check tailnet chat: done (dialled 1, 1 received"),
                   what="the check's log line", timeout=10)
        log = dev.log_text()
        for line in ("holding the device awake: delivering", "deliveries over",
                     "closed laptop (", "nothing more waits"):
            assert line in log, f"no {line!r} in the log"

        # The interval is a setting.
        target.call({"cmd": "set", "check_minutes": 30})
        system = json.loads((dev.card / "config" / "system.json").read_text())
        assert system.get("thistle_os", system)["tailnet_chat"]["check_minutes"] == 30, system
        target.close()
    finally:
        laptop.stop()
        dev.stop()


def ppm_has_text(path: Path) -> bool:
    data = path.read_bytes()
    # P6 header: three lines, then the pixels; a screen of one colour is blank.
    body = data.split(b"\n", 3)[3]
    return len(set(body[i] for i in range(0, len(body), 97))) > 1


def case_app(simulator: Path, root: Path) -> None:
    dev = Device(simulator, root, "app")
    key_hex = ed25519.public_key(os.urandom(32)).hex()
    fp = protocol.fingerprint_id(bytes.fromhex(key_hex))
    requests = dev.card / "messages" / "tailnet" / "requests"
    requests.mkdir(parents=True)
    (requests / f"{fp}.jsonl").write_text(json.dumps({
        "dir": "in", "from": key_hex, "name": "laptop", "id": "ab" * 16, "ts": 1_793_541_600_000,
        "time": None, "text": "Hello from the laptop", "state": "received"}) + "\n")

    shot = root / "requests.ppm"
    log = dev.run(["--launch-app", APP, "--tap", TAB_REQUESTS, "--tap", FIRST_ROW], 8000, shot)
    for line in ("(tailchat) on_create", "(tailchat) page requests: 1 rows",
                 f"(tailchat) request laptop {protocol.fingerprint(bytes.fromhex(key_hex))}: 1 held"):
        assert line in log, f"no {line!r}: {log[-1500:]}"
    assert ppm_has_text(shot), "the Requests screen is blank"

    # Accept from the request's page, which opens the conversation; back
    # from it is Requests.
    log = dev.run(["--launch-app", APP, "--tap", TAB_REQUESTS, "--tap", FIRST_ROW, "--tap", ACCEPT,
                   "--tap", "back", "--tap", TAB_CONTACTS], 12500)
    assert "[tailnet_chat] accepted laptop" in log, log[-1500:]
    assert "(tailchat) conversation laptop: 1 messages" in log, log[-1500:]
    assert "(tailchat) page contacts: 1 rows" in log, log[-1500:]
    assert (dev.card / "messages" / "tailnet" / f"{fp}.jsonl").is_file()
    saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
    assert [c["key"] for c in saved["contacts"]] == [key_hex]

    off = Device(simulator, root, "app-off", tailnet=False)
    log = off.run(["--launch-app", APP], 5000)
    assert "(tailchat) unavailable: Tailscale is off" in log, log[-1500:]


def case_app_add_contact(simulator: Path, root: Path) -> None:
    """Story 2 on the device: a contact added by its tailnet name from the
    app and written to from its page, and the check's interval set there."""
    dev = Device(simulator, root, "app-add")
    # The interval first, in Settings.
    log = dev.run(["--launch-app", APP, "--tap", SETTINGS, "--tap", CHECK_INTERVAL, "--tap", "wait"], 6000)
    assert "(tailchat) check every 30 minutes" in log, log[-3000:]
    log = dev.run(["--launch-app", APP, "--tap", TAB_CONTACTS, "--tap", ADD_CONTACT,
                   "--tap", "type:homelab", "--tap", ADD, "--tap", "wait"], 9000)
    for line in ("(tailchat) added homelab port 0",
                 "[tailnet_chat] added homelab at homelab:4810", "(tailchat) contact homelab"):
        assert line in log, f"no {line!r} in the log:\n{log[-3000:]}"
    saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
    assert [(c["name"], c.get("dial"), c.get("pending")) for c in saved["contacts"]] == \
        [("homelab", ["homelab", 4810], True)], saved
    # Written to from its page: Chats lists it only once it has messages.
    log = dev.run(["--launch-app", APP, "--tap", TAB_CONTACTS, "--tap", CONTACT_ROW, "--tap", MESSAGE,
                   "--tap", "type:are you there\n", "--tap", "wait"], 9000)
    for line in ("(tailchat) conversation homelab: 0 messages", "(tailchat) sent ",
                 "(tailchat) conversation homelab: 1 messages"):
        assert line in log, f"no {line!r} in the log:\n{log[-3000:]}"
    system = json.loads((dev.card / "config" / "system.json").read_text())
    assert system.get("thistle_os", system)["tailnet_chat"]["check_minutes"] == 30, system


def case_app_conversation(simulator: Path, root: Path) -> None:
    """A full conversation on screen, with replies, reactions and Arabic:
    LVGL's pool measured; read on opening; a message typed and sent."""
    dev = Device(simulator, root, "conversation")
    key_hex = ed25519.public_key(os.urandom(32)).hex()
    fp = protocol.fingerprint_id(bytes.fromhex(key_hex))
    (dev.spiffs / "tailnet_chat.json").write_text(json.dumps({
        "version": 1, "outbox": [],
        "contacts": [{"name": "Lena", "key": key_hex, "seen_name": "lena-laptop", "unread": 2}]}))
    history = dev.card / "messages" / "tailnet"
    history.mkdir(parents=True)
    lines, ids = [], []
    t0 = 1_793_541_600_000
    for i in range(24):
        mine = i % 3 == 0
        mid = f"{i:032x}"
        ids.append(mid)
        text = ("مرحبا، كيف حالك اليوم؟ " if i % 4 == 1 else f"Message {i} ") * (40 if i % 5 == 0 else 3)
        line = {"dir": "out" if mine else "in", ("to" if mine else "from"): key_hex, "name": "Lena", "id": mid,
                "ts": t0 + i * 60_000, "time": None, "text": text, "state": "waiting" if mine else "received"}
        if i % 6 == 5:
            line["reply_to"] = ids[i - 2]
        lines.append(line)
        if mine:
            lines.append({"dir": "out", "to": key_hex, "id": mid, "state": "read", "time": None})
        if i % 4 == 2:
            lines.append({"dir": "in", "from": key_hex, "name": "Lena", "id": f"{i + 100:032x}", "ts": t0 + i,
                          "react": ids[i - 1], "emoji": "👍", "time": None})
    (history / f"{fp}.jsonl").write_text("".join(json.dumps(x, ensure_ascii=False) + "\n" for x in lines))

    shot = root / "conversation.ppm"
    log = dev.run(["--launch-app", APP, "--tap", FIRST_ROW, "--tap", "type:hello there\n", "--tap", "wait"],
                  9000, shot)
    pool = [x for x in log.splitlines() if "(tailchat) conversation Lena: 20 messages; LVGL pool" in x]
    assert pool, log[-2000:]
    free_kb = int(pool[0].split("LVGL pool ")[1].split(" KB")[0])
    print(f"      {pool[0].split('(tailchat) ')[1].split(chr(27))[0]}")
    assert free_kb >= 8, f"LVGL's pool is nearly dry with a full conversation open: {free_kb} KB free"
    assert ppm_has_text(shot), "the conversation is blank"
    assert "(statusbar) unread 2" in log and "(statusbar) unread 0" in log, "not read on opening"
    sent = [x for x in log.splitlines() if "(tailchat) sent " in x]
    assert sent, log[-2000:]
    saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
    waiting = [json.loads(f["frame"]) for b in saved["outbox"] for f in b["frames"]]
    assert [w["text"] for w in waiting] == ["hello there"], waiting
    last = json.loads((history / f"{fp}.jsonl").read_text().splitlines()[-1])
    assert last["dir"] == "out" and last["text"] == "hello there" and last["state"] == "waiting", last
    assert saved["contacts"][0].get("unread", 0) == 0


VECTORS = REPO / "docs" / "tailnet-chat" / "vectors"


def tone_wav(path: Path, seconds: float) -> None:
    """A 16 kHz mono WAV of a voice-like tone: what the microphone hears."""
    import math
    import struct
    import wave
    rate = 16000
    frames = bytearray()
    for i in range(int(rate * seconds)):
        t = i / rate
        level = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * t)   # syllables
        s = level * (math.sin(2 * math.pi * 220 * t) + 0.5 * math.sin(2 * math.pi * 660 * t))
        frames += struct.pack("<h", int(8000 * s))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def voice_device(simulator: Path, root: Path, name: str, history: list) -> tuple[Device, str, str]:
    """A device whose one contact, Lena, has `history` lines in her
    conversation: (device, her key, her fingerprint id)."""
    dev = Device(simulator, root, name)
    key_hex = ed25519.public_key(os.urandom(32)).hex()
    fp = protocol.fingerprint_id(bytes.fromhex(key_hex))
    (dev.spiffs / "tailnet_chat.json").write_text(json.dumps({
        "version": 1, "outbox": [],
        "contacts": [{"name": "Lena", "key": key_hex, "seen_name": "lena-laptop"}]}))
    folder = dev.card / "messages" / "tailnet"
    folder.mkdir(parents=True)
    lines = [dict(line, **{"from" if line["dir"] == "in" else "to": key_hex}) for line in history]
    (folder / f"{fp}.jsonl").write_text("".join(json.dumps(x) + "\n" for x in lines))
    return dev, key_hex, fp


# The conversation page on the T-Deck Max with one message, under its day's
# heading: its bubble, which plays when tapped if it is a voice message, and
# the microphone at the composer's right.
CONV_ROW = "60,124"
VOICE_BUTTON = "217,299"


def case_voice(simulator: Path, root: Path) -> None:
    """A voice message recorded by holding the voice button, from
    THISTLE_SIM_MIC, kept on the card as Ogg/Opus and queued; a tap is not
    one; a received one plays only when chosen, to THISTLE_SIM_SPEAKER."""
    sys.path.insert(0, str(REPO / "tools"))
    from tailnet_chat import ogg

    # Recorded and sent.
    dev, key_hex, fp = voice_device(simulator, root, "voice-record", [
        {"dir": "in", "name": "Lena", "id": "1" * 32, "ts": 1_793_541_600_000, "time": None, "text": "say it",
         "state": "received"}])
    mic = root / "mic.wav"
    tone_wav(mic, 6.0)
    os.environ["THISTLE_SIM_MIC"] = str(mic)
    try:
        shot = root / "voice-record.ppm"
        log = dev.run(["--launch-app", APP, "--tap", FIRST_ROW, "--tap", f"{VOICE_BUTTON}:120",
                       "--tap", f"{VOICE_BUTTON}:3000", "--tap", "wait", "--tap", "wait"], 14000, shot)
    finally:
        os.environ.pop("THISTLE_SIM_MIC")
    assert "(tailchat) no voice message: Hold to record a voice message" in log, log[-3000:]
    sent = [x for x in log.splitlines() if "(tailchat) sent voice " in x]
    assert len(sent) == 1, log[-3000:]
    print(f"      {sent[0].split('(tailchat) ')[1].split(chr(27))[0]}")
    mid = sent[0].split("sent voice ")[1].split(":")[0]
    saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
    frames = [json.loads(f["frame"]) for b in saved["outbox"] for f in b["frames"]]
    assert len(frames) == 1 and frames[0]["type"] == "voice" and frames[0]["id"] == mid, frames
    voice = frames[0]
    assert voice["codec"] == "ogg-opus" and 1500 <= voice["ms"] <= 3200, voice
    path = dev.card / "messages" / "tailnet" / "voice" / f"{mid}.opus"
    data = path.read_bytes()
    assert len(data) == voice["bytes"], (len(data), voice)
    opus = ogg.read_opus(data)
    assert opus.channels == 1 and opus.input_rate == 16000, opus
    assert abs(opus.duration_ms - voice["ms"]) <= 20, (opus.duration_ms, voice["ms"])
    assert len(opus.frames) == voice["ms"] // 20
    last = json.loads(dev.chat_file(key_hex).read_text().splitlines()[-1])
    assert last["voice"] == f"voice/{mid}.opus" and last["ms"] == voice["ms"] and last["state"] == "waiting", last
    assert not list(path.parent.glob("*.tmp")), "a half-written file was left"

    # Received, and played when chosen.
    rid = "2" * 32
    dev, key_hex, fp = voice_device(simulator, root, "voice-play", [
        {"dir": "in", "name": "Lena", "id": rid, "ts": 1_793_541_600_000, "time": None, "text": "",
         "voice": f"voice/{rid}.opus", "ms": 1500, "bytes": 6129, "state": "received"}])
    (dev.card / "messages" / "tailnet" / "voice").mkdir()
    shutil.copy(VECTORS / "voice.opus", dev.card / "messages" / "tailnet" / "voice" / f"{rid}.opus")
    speaker = root / "speaker.wav"
    os.environ["THISTLE_SIM_SPEAKER"] = str(speaker)
    try:
        shot = root / "voice-play.ppm"
        log = dev.run(["--launch-app", APP, "--tap", FIRST_ROW, "--tap", "wait", "--tap", CONV_ROW,
                       "--tap", "wait", "--tap", "wait"], 12000, shot)
    finally:
        os.environ.pop("THISTLE_SIM_SPEAKER")
    assert "(tailchat) chose " + rid in log, log[-3000:]
    assert f"(tailchat_voice) playing {rid}: 1500 ms" in log, log[-3000:]
    assert "(tailchat) playing: nothing" in log, "it did not finish"
    import wave
    with wave.open(str(speaker)) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2)
        pcm = w.readframes(w.getnframes())
    ms = len(pcm) // 32
    assert 1400 <= ms <= 1600, f"{ms} ms played"
    import array
    samples = array.array("h", pcm)
    rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
    assert rms > 300, f"the speaker heard near silence: RMS {rms:.0f}"
    print(f"      played {ms} ms of {rid[:8]}..., RMS {rms:.0f}")
def wav_samples(path: Path) -> list[int]:
    """The 16-bit samples of a WAV file the simulator's speaker wrote."""
    import struct
    data = path.read_bytes()
    body = data[44:]
    return list(struct.unpack(f"<{len(body) // 2}h", body[: len(body) // 2 * 2]))


def case_calls(simulator: Path, root: Path) -> None:
    """The conformance suite's call scenarios against the simulator, the
    cellular one through the fake modem's --call, and an offer of webrtc
    and ws-opus answered with ws-opus; the call screen by
    screenshot; and the speaker playing what the reference peer sent."""
    dev = Device(simulator, root, "calls")
    speaker = root / "speaker.wav"
    dev.start(THISTLE_SIM_SPEAKER=speaker)
    try:
        target = SimTarget(dev)
        out = []

        class Out:
            def write(self, s):
                out.append(s)

        skipped: list = []
        failed = conformance.run(lambda: target, CALLS, out=Out(), skipped=skipped)
        print("      " + "".join(out).strip().replace("\n", "\n      "))
        assert failed == 0 and not skipped, f"{failed} call scenarios failed, {len(skipped)} skipped"
        log = dev.log_text()
        for line in ("[tailnet_call] call ", " over ws-opus", "ended: hangup", "ended: cellular",
                     "ended: timeout", "ended: unreachable", "frames lost"):
            assert line in log, f"no {line!r} in the log"

        # A call on screen: the contact's name and the call's state; the
        # speaker plays what the peer sends. The last call's screen first
        # says how it ended, and closes.
        time.sleep(3.5)
        conn, call, stream = conformance._connected_call(target, "Lena")
        played_from = speaker.stat().st_size if speaker.exists() else 44
        conformance._speak(conn, stream, 60)
        shot = root / "call.ppm"
        target.screenshot(shot)
        assert ppm_has_text(shot), "the call screen is blank"
        if os.environ.get("TAILCHAT_SHOTS"):   # to look at it
            shutil.copy(shot, Path(os.environ["TAILCHAT_SHOTS"]) / "call.ppm")
        assert target.logged("(call_ui) open incoming"), "the call screen did not open"
        assert target.logged("(call_ui) tailnet call: Lena"), "the call screen did not show the contact"
        assert target.calls()[call]["frames"] >= 50
        conn.send({"type": "call_end", "call": call, "reason": "hangup"})
        eventually(lambda: conformance._call_state(target, call) == "ended:hangup", what="the call to end")
        eventually(lambda: target.logged("[tailnet_call] played "), what="the call task's summary")
        heard = wav_samples(speaker)[max(0, (played_from - 44) // 2):]
        loud = [x for x in heard if abs(x) > 300]
        assert len(loud) > 16_000 // 4, f"the speaker played {len(loud)} loud samples of the peer's voice"
        conn.close()
        target.close()
    finally:
        dev.stop()


def case_caps(simulator: Path, root: Path) -> None:
    """Protocol section 5: a peer uses only what both hellos announce. To a
    contact that announced `text` alone, a reply goes without its reply_to,
    a reaction stays queued, and a call ends at once as unsupported, with
    nothing sent."""
    dev = Device(simulator, root, "caps").start()
    try:
        target = SimTarget(dev)
        plain = RawConn(target.address, name="plain", caps=["text"]).handshake()
        conformance._become_contact(target, plain)
        m = msg("plain text only")
        plain.send(m)
        plain.expect("ack", id=m["id"])
        mid = target.send_text(plain.key_hex, "a reply", reply_to=m["id"])
        got = plain.expect("msg", id=mid)
        assert "reply_to" not in got, got
        plain.send({"type": "ack", "id": mid})
        rid = target.react(plain.key_hex, m["id"], "👍")
        call = target.place_call(plain.key_hex)
        eventually(lambda: conformance._call_state(target, call) == "ended:unsupported", what="the call to end")
        seen = plain.drain(1.0)
        kinds = [f["type"] for f in seen if isinstance(f, dict)]
        assert "react" not in kinds and "call_offer" not in kinds, kinds
        assert target.waiting(plain.key_hex) == [rid], target.waiting(plain.key_hex)
        plain.close()
        target.close()
    finally:
        dev.stop()


def case_no_history(simulator: Path, root: Path) -> None:
    """With "save history" off nothing goes on the card, yet a message sent
    again after a restart is still known, and a contact request still holds
    what came with it (story 5): both are the module's own, on SPIFFS."""
    dev = Device(simulator, root, "nohistory", history=False).start()
    try:
        target = SimTarget(dev)
        pal = RawConn(target.address, name="pal").handshake()
        conformance._become_contact(target, pal)
        m = msg("said once")
        pal.send(m)
        pal.expect("ack", id=m["id"])
        theirs = {"type": "react", "id": os.urandom(16).hex(), "ts": int(time.time() * 1000), "to": m["id"],
                  "emoji": "🎉"}
        pal.send(theirs)
        pal.expect("ack", id=theirs["id"])
        pal.close()
        stranger = RawConn(target.address, name="stranger").handshake()
        held = msg("held across a restart")
        stranger.send(held)
        stranger.expect("ack", id=held["id"])
        stranger.close()
        target.call({"cmd": "mark_read", "to": pal.key_hex})
        target.close()
        dev.stop()
        assert not (dev.card / "messages" / "tailnet").exists() or \
            not any((dev.card / "messages" / "tailnet").rglob("*.jsonl")), "history written with it off"

        dev.start()
        target = SimTarget(dev)
        assert [x["text"] for r in target.requests() if r["key"] == stranger.key_hex for x in r["held"]] == \
            [held["text"]], target.requests()
        again = RawConn(target.address, name="pal", seed=pal.seed).handshake()
        again.send(m)
        again.expect("ack", id=m["id"])
        again.send(theirs)
        again.expect("ack", id=theirs["id"])
        time.sleep(0.3)
        assert m["id"] not in [x["id"] for x in target.messages(pal.key_hex)], "shown again after the restart"
        assert target.call({"cmd": "unread"}) == 0
        assert "received " + m["id"] + " from pal again" in dev.log_text()
        again.close()
        target.accept(stranger.key_hex)
        shown = [x["text"] for x in target.messages(stranger.key_hex)]
        assert shown == [held["text"]], shown
        target.close()
    finally:
        dev.stop()


def case_app_discovery(simulator: Path, root: Path) -> None:
    """Ticket 13: Look for chat peers probes the simulator's tailnet peers,
    two reference chat peers, a port that answers HTTP and the simulator
    itself, lists the two, and adds the first by the address it answered
    on, its key pinned at the first handshake. A probe leaves nothing on
    the side probed."""
    port = free_port()
    dev = Device(simulator, root, "discovery")
    system_path = dev.card / "config" / "system.json"
    system = json.loads(system_path.read_text())
    system.get("thistle_os", system)["tailnet_chat"] = {"port": port}
    system_path.write_text(json.dumps(system, indent=2))
    alice = ChatPeer(name="alice-laptop", host="127.0.0.2", port=port).start()
    bob = ChatPeer(name="bob-desktop", host="127.0.0.3", port=port).start()
    stop = threading.Event()

    def not_chat() -> None:
        with socket.socket() as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("127.0.0.4", port))
            srv.listen()
            srv.settimeout(0.2)
            while not stop.is_set():
                try:
                    c, _ = srv.accept()
                except OSError:
                    continue
                with c:
                    c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")

    printer = threading.Thread(target=not_chat, daemon=True)
    printer.start()
    peers = "alice=127.0.0.2,bob=127.0.0.3,printer=127.0.0.4,thistle-sim=127.0.0.1"
    try:
        shot = root / "discovery.ppm"
        log = dev.run(["--launch-app", APP, "--tap", TAB_CONTACTS, "--tap", LOOK, "--tap", "wait",
                       "--tap", "wait"], 9000, shot, THISTLE_SIM_TAILNET_PEERS=peers, THISTLE_SIM_CHAT_PORT=port)
        for line in ("[tailnet_chat] looking for chat peers: 3 of 4 tailnet peers to ask",
                     f"alice.tail-sim.ts.net at 127.0.0.2:{port} is a chat peer: alice-laptop {alice.fingerprint}",
                     f"bob.tail-sim.ts.net at 127.0.0.3:{port} is a chat peer: bob-desktop {bob.fingerprint}",
                     f"printer.tail-sim.ts.net at 127.0.0.4:{port}: no chat peer answered",
                     "found 2 chat peers among 3 tailnet peers",
                     f"(tailchat) found alice-laptop (alice) {alice.fingerprint}",
                     f"(tailchat) found bob-desktop (bob) {bob.fingerprint}",
                     "(tailchat) page find: done, 3 of 3 asked, 2 rows"):
            assert line in log, f"no {line!r} in the log:\n{log[-3000:]}"
        assert ppm_has_text(shot), "the chat peers found are not shown"
        # Probed only: no handshake, nothing held.
        assert not [e for e in alice.events + bob.events if e[0] in ("connected", "contact request")], \
            alice.events + bob.events
        assert not alice.requests and not bob.requests

        # Add the first: by the address it answered on, pinned when reached.
        log = dev.run(["--launch-app", APP, "--tap", TAB_CONTACTS, "--tap", LOOK, "--tap", "wait",
                       "--tap", "wait", "--tap", FIRST_FOUND, "--tap", "wait", "--tap", "wait"], 12000,
                      THISTLE_SIM_TAILNET_PEERS=peers, THISTLE_SIM_CHAT_PORT=port)
        for line in (f"[tailnet_chat] added alice-laptop at 127.0.0.2:{port}",
                     "(tailchat) added alice-laptop, found on the tailnet",
                     f"pinned alice-laptop as {alice.fingerprint}"):
            assert line in log, f"no {line!r} in the log:\n{log[-3000:]}"
        saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
        assert [(c["name"], c["key"]) for c in saved["contacts"]] == [("alice-laptop", alice.key_hex)], saved
        # Looking again, the contact is left out.
        shot2 = root / "discovery-again.ppm"
        log = dev.run(["--launch-app", APP, "--tap", TAB_CONTACTS, "--tap", LOOK, "--tap", "wait",
                       "--tap", "wait"], 9000, shot2, THISTLE_SIM_TAILNET_PEERS=peers, THISTLE_SIM_CHAT_PORT=port)
        assert "(tailchat) page find: done, 3 of 3 asked, 1 rows" in log, log[-3000:]
        assert "(tailchat) found bob-desktop" in log and "(tailchat) found alice-laptop" not in log, log[-3000:]
        assert not bob.requests
    finally:
        stop.set()
        alice.stop()
        bob.stop()


CASES: list[tuple[str, Callable[[Path, Path], None]]] = [
    ("conformance", case_conformance),
    ("card-and-restart", case_card_and_restart),
    ("messages", case_messages),
    ("sleep-check", case_sleep_check),
    ("app", case_app),
    ("app-conversation", case_app_conversation),
    ("voice", case_voice),
    ("app-add-contact", case_app_add_contact),
    ("app-discovery", case_app_discovery),
    ("calls", case_calls),
    ("caps", case_caps),
    ("no-history", case_no_history),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run the case of this name alone")
    args = parser.parse_args()
    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Tailnet chat ===")
    failures = 0
    cases = [c for c in CASES if not args.only or c[0] == args.only]
    with tempfile.TemporaryDirectory(prefix="thistle-tailchat-") as tmp:
        for name, case in cases:
            try:
                case(simulator, Path(tmp))
            except Exception:  # noqa: BLE001
                failures += 1
                print(f"FAIL  {name}:\n{traceback.format_exc()}")
                for log in Path(tmp).glob("*.log"):
                    print(f"--- {log.name} (end) ---\n{log.read_text(errors='replace')[-3000:]}")
            else:
                print(f"PASS  {name}")
    if failures:
        print(f"{failures}/{len(cases)} Tailnet chat tests failed")
        return 1
    print(f"All {len(cases)} Tailnet chat tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
