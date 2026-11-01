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
  unavailable when Tailscale is off.

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

APP = "com.thistle.tailchat"
SCENARIOS = [conformance.handshake, conformance.bad_signature_refused, conformance.unknown_type_answered,
             conformance.invalid_frames_answered, conformance.contact_request_accepted,
             conformance.contact_request_declined, conformance.key_changed_refused]

# The app's tabs and first row, on the T-Deck Max.
TAB_REQUESTS = "195,40"
TAB_CONTACTS = "120,40"
FIRST_ROW = "120,95"
# On the request's page, below a message of one line.
ACCEPT = "42,195"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Device:
    """A simulator on a scratch card and SPIFFS, which a restart keeps."""

    def __init__(self, simulator: Path, root: Path, name: str, tailnet: bool = True):
        self.simulator = simulator
        self.card = root / name
        self.spiffs = root / f"{name}-spiffs"
        shutil.copytree(SIM_DIR / "sdcard", self.card)
        self.spiffs.mkdir()
        (self.spiffs / ".tailnet_joined").write_text("1")
        system_path = self.card / "config" / "system.json"
        system = json.loads(system_path.read_text())
        system.get("thistle_os", system)["tailnet"] = {"enabled": tailnet}
        system_path.write_text(json.dumps(system, indent=2))
        self.proc: Optional[subprocess.Popen] = None
        self.log_path = root / f"{name}.log"

    def env(self, **extra) -> dict:
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(self.card), THISTLE_SIM_SPIFFS=str(self.spiffs))
        for name in ("THISTLE_SIM_TAILNET", "THISTLE_SIM_TAILNET_PEERS", "THISTLE_SIM_TAILNET_LOGIN_S"):
            env.pop(name, None)
        env.update({k: str(v) for k, v in extra.items()})
        return env

    def start(self) -> "Device":
        self.port, self.control_port = free_port(), free_port()
        self.log = open(self.log_path, "a")
        self.proc = subprocess.Popen(
            [str(self.simulator), "--headless", "--device", "tdeck-max", "--timeout", "600000"],
            env=self.env(THISTLE_SIM_CHAT_PORT=self.port, THISTLE_SIM_CHAT_CONTROL=self.control_port),
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

    def run(self, args: list[str], timeout_ms: int, screenshot: Optional[Path] = None) -> str:
        """A run of its own with taps, to its end: its log."""
        command = [str(self.simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(timeout_ms), *args]
        if screenshot:
            command += ["--screenshot", str(screenshot)]
        result = subprocess.run(command, env=self.env(THISTLE_SIM_CHAT_PORT=free_port()), text=True,
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

    def close(self) -> None:
        self.sock.close()


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
                     "not the key pinned for laptop-"):
            assert line in log, f"no {line!r} in the log"
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

    # Accept from the request's page: Accept is its first button.
    log = dev.run(["--launch-app", APP, "--tap", TAB_REQUESTS, "--tap", FIRST_ROW, "--tap", ACCEPT,
                   "--tap", TAB_CONTACTS], 11000)
    assert "[tailnet_chat] accepted laptop" in log, log[-1500:]
    assert "(tailchat) page contacts: 1 rows" in log, log[-1500:]
    assert (dev.card / "messages" / "tailnet" / f"{fp}.jsonl").is_file()
    saved = json.loads((dev.spiffs / "tailnet_chat.json").read_text())
    assert [c["key"] for c in saved["contacts"]] == [key_hex]

    off = Device(simulator, root, "app-off", tailnet=False)
    log = off.run(["--launch-app", APP], 5000)
    assert "(tailchat) unavailable: Tailscale is off" in log, log[-1500:]


CASES: list[tuple[str, Callable[[Path, Path], None]]] = [
    ("conformance", case_conformance),
    ("card-and-restart", case_card_and_restart),
    ("app", case_app),
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
