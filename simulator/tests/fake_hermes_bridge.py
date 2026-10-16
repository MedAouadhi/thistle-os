#!/usr/bin/env python3
"""A Hermes Device Bridge for the simulator's Hermes app to talk to.

It speaks protocol version 1 as hermes_esp32's plugin does
(plugins/platforms/hermes_device/bridge/service.py): the same frames, in the
same order, with json.dumps's compact separators, so everything past ASCII
arrives escaped. It answers a CHAT_SUBMIT the way Hermes does through the
plugin: CHAT_ACCEPTED, the user's message echoed, a tool's activity row, and
a reply that is edited as it streams. It also pings, as the real one does
every 20 seconds, and counts the pongs.

The WebSocket is the standard library's sockets and RFC 6455 by hand, so the
suite needs nothing installed.

    bridge = FakeBridge(mode="normal")   # or refuse, incompatible, drop, busy
    bridge.start()
    ... ws://127.0.0.1:{bridge.port} ...
    bridge.stop()
    bridge.received   # every frame the device sent, as dicts
"""

from __future__ import annotations

import base64
import hashlib
import json
import socket
import struct
import threading
import time
from typing import Optional

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CONVERSATION = "078c84476ae445b58bb9dca12c80e51e"
OTHER_CONVERSATION_SESSION = "20261015_180000_cd34"

HISTORY = [
    ("h1", "user", "Is the heating on?"),
    ("h2", "assistant", "Yes. The living room is at **21°C**, set to 21°C.\n\n"
                        "| Room | °C |\n| --- | ---: |\n| Living | 21 |"),
]

COMMANDS = [
    {"name": name, "description": description, "args_hint": hint}
    for name, description, hint in (
        ("new", "Start a new conversation", ""),
        ("stop", "Stop the active response", ""),
        ("model", "Switch the model for this session", "[name]"),
        ("fast", "Faster replies — for a cost", ""),
        ("compress", "Compress the context", ""),
        ("usage", "Show token usage", ""),
        ("title", "Name this session", "[title]"),
        ("retry", "Retry the last message", ""),
        ("undo", "Remove the last exchange", ""),
        ("verbose", "Cycle tool progress", ""),
        ("yolo", "Approve everything this session", ""),
        ("resume", "Resume a named session", "[name]"),
        ("sessions", "List sessions", ""),
        ("help", "List commands", ""),
    )
]

# A reply with what Hermes writes: a heading, a table that fits, one that
# does not, a list, code, Arabic, and characters the fonts lack.
REPLY = (
    "## Prices today\n\n"
    "| Hour | Price |\n| --- | ---: |\n| 06:00 | 6.20 |\n| 07:00 | 6.40 |\n\n"
    "| Hour | Price | Trend | Grid fee | Total |\n| --- | --- | --- | --- | --- |\n"
    "| 08:00 | 5.90 c/kWh | ▼ | 3.10 c/kWh | 9.00 c/kWh |\n\n"
    "- Cheapest at **08:00** — run the dishwasher then ✅\n"
    "- Most expensive at 18:00\n\n"
    "```\ncharge --at 08:00\n```\n\n"
    "مرحبا بك"
)
STREAMED = "Looking at today’s prices…"

SESSIONS = [
    {"id": "20261016_093000_ab12", "label": "Electricity prices", "messages": 4,
     "age_seconds": 30, "current": True},
    {"id": OTHER_CONVERSATION_SESSION, "label": "Heating schedule", "messages": 12,
     "age_seconds": 55000, "current": False},
    {"id": "20261010_070000_ef56", "label": "Oct 10 07:00", "messages": 2,
     "age_seconds": 520000, "current": False},
]
OTHER_HISTORY = [
    ("o1", "user", "Lower the heating at night"),
    ("o2", "assistant", "Done: 18°C from 23:00 to 06:00."),
]

JOBS = [
    {"job_id": "b7c2d4e91a05", "name": "Blog monitor", "schedule": "every 6 hours",
     "age_seconds": 1380, "unread": 1},
    {"job_id": "a3f19c0e77b4", "name": "Morning price summary",
     "schedule": "every day at 06:30", "age_seconds": 19800, "unread": 2},
]


def run_for(offset: int) -> Optional[dict]:
    if offset >= 2:
        return None
    return {
        "captured": f"2026-10-{16 - offset} 06:30",
        "age_seconds": 19800 + offset * 86400,
        "truncated": False,
        "parts_dropped": False,
        "older": offset < 1,
        "parts": [f"## Morning price summary {16 - offset}\n\nCheapest hour: 0{offset + 3}:00"],
    }


class Connection:
    def __init__(self, bridge: "FakeBridge", sock: socket.socket) -> None:
        self.bridge = bridge
        self.sock = sock
        self.lock = threading.Lock()
        self.closed = False
        self.position = 0

    # -- framing ---------------------------------------------------------

    def handshake(self) -> bool:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(1024)
            if not chunk:
                return False
            data += chunk
        head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
        headers = {}
        for line in head.split("\r\n")[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        key = headers.get("sec-websocket-key", "")
        self.bridge.request_lines.append(head.split("\r\n")[0])
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.sock.sendall((
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        return True

    def send_frame(self, opcode: int, payload: bytes) -> None:
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack(">H", n)
        else:
            head += bytes([127]) + struct.pack(">Q", n)
        with self.lock:
            if not self.closed:
                try:
                    self.sock.sendall(head + payload)
                except OSError:
                    self.closed = True

    def send(self, message: dict) -> None:
        self.send_frame(0x1, json.dumps(message, separators=(",", ":")).encode())

    def read_exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError
            data += chunk
        return data

    def read_frame(self) -> tuple[int, bytes]:
        b0, b1 = self.read_exact(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.read_exact(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.read_exact(8))[0]
        if not b1 & 0x80:
            self.bridge.unmasked += 1
        mask = self.read_exact(4) if b1 & 0x80 else b"\0\0\0\0"
        payload = bytes(c ^ mask[i % 4] for i, c in enumerate(self.read_exact(n)))
        return b0 & 0x0F, payload

    def close(self, code: int, reason: str = "") -> None:
        self.send_frame(0x8, struct.pack(">H", code) + reason.encode())
        with self.lock:
            self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    # -- the bridge ------------------------------------------------------

    def message(self, mid: str, role: str, text: str, position: int, **extra) -> dict:
        frame = {"type": "CHAT_MESSAGE",
                 "message": {"id": mid, "role": role, "text": text, "position": position,
                             "part": 0, "parts": 1}}
        frame.update(extra)
        return frame

    def sync(self, history) -> None:
        self.position = len(history)
        self.send({"type": "CHAT_SYNC", "conversation_id": CONVERSATION,
                   "acknowledged_position": self.position, "history": "loaded",
                   "messages": [], "commands": []})
        for position, (mid, role, text) in enumerate(history, 1):
            self.send(self.message(mid, role, text, position))
        for offset in range(0, len(COMMANDS), 12):
            self.send({"type": "CHAT_COMMANDS", "replace": offset == 0,
                       "commands": COMMANDS[offset:offset + 12]})
        self.send({"type": "CHAT_SYNC_COMPLETE"})

    def answer(self, request_id: str, text: str) -> None:
        self.send({"type": "CHAT_ACCEPTED", "request_id": request_id})
        self.position += 1
        self.send(self.message(request_id, "user", text, self.position))
        tool = {"id": "tool-1", "label": "Reading prices", "detail": "nordpool today",
                "state": "running"}
        self.send({"type": "CHAT_ACTIVITY", "visibility": "all", "activity": tool})
        if text.startswith("slow"):
            return  # a turn that runs until it is stopped
        time.sleep(0.3)
        self.position += 1
        reply_id = "r" + request_id[:8]
        self.send(self.message(reply_id, "assistant", STREAMED, self.position))
        time.sleep(0.2)
        self.send({"type": "CHAT_MESSAGE_REPLACE", "message_id": reply_id, "final": False})
        self.send(self.message(reply_id, "assistant", STREAMED + " still", self.position,
                               final=False))
        time.sleep(0.2)
        self.send({"type": "CHAT_ACTIVITY", "visibility": "all",
                   "activity": dict(tool, label="Read prices", detail="", state="completed")})
        self.send({"type": "CHAT_MESSAGE_REPLACE", "message_id": reply_id, "final": True})
        self.send(self.message(reply_id, "assistant", REPLY, self.position, final=True))

    def handle(self, message: dict) -> None:
        kind = message.get("type")
        if kind == "CHAT_SUBMIT":
            threading.Thread(target=self.answer,
                             args=(message["request_id"], message["text"]), daemon=True).start()
        elif kind == "CHAT_CONTROL":
            if message.get("action") == "new":
                self.position = 0
                self.send({"type": "CHAT_RESET", "request_id": message["request_id"],
                           "conversation_id": CONVERSATION})
            else:
                self.send({"type": "CHAT_ACCEPTED", "request_id": message["request_id"]})
                self.send({"type": "CHAT_ACTIVITY", "visibility": "all",
                           "activity": {"id": "tool-1", "label": "Reading prices",
                                        "detail": "", "state": "failed"}})
        elif kind == "SESSION_LIST_REQUEST":
            self.send({"type": "SESSION_LIST", "sessions": SESSIONS})
        elif kind == "SESSION_SELECT":
            self.send({"type": "CHAT_RESET", "request_id": message["request_id"],
                       "conversation_id": CONVERSATION})
            self.sync(OTHER_HISTORY)
        elif kind == "SCHEDULED_JOBS_REQUEST":
            self.send({"type": "SCHEDULED_JOBS", "jobs": JOBS})
        elif kind == "SCHEDULED_RUN_REQUEST":
            self.send({"type": "SCHEDULED_RUN", "job_id": message["job_id"],
                       "offset": message["offset"], "run": run_for(message["offset"])})
        else:
            self.send({"type": "ERROR", "code": "UNSUPPORTED_CONTROL_MESSAGE",
                       "message": "The control message is not available in this Bridge Session."})

    def pinger(self) -> None:
        while not self.closed:
            time.sleep(0.5)
            self.send_frame(0x9, b"ping")
            self.bridge.pings += 1

    def run(self) -> None:
        bridge = self.bridge
        try:
            if not self.handshake():
                return
            bridge.connections += 1
            if bridge.mode == "refuse":
                self.send({"type": "ERROR", "code": "AUTHENTICATION_FAILED",
                           "message": "This tailnet node is not an authorized Hermes device."})
                self.close(4401, "tailnet untagged")
                return
            opcode, payload = self.read_frame()
            hello = json.loads(payload)
            bridge.received.append(hello)
            if bridge.mode == "incompatible":
                self.send({"type": "ERROR", "code": "PROTOCOL_INCOMPATIBLE",
                           "message": "No common protocol version: device supports 1-1, "
                                      "bridge supports 2-2. Update the plugin.",
                           "update": "plugin", "supported": {"min": 2, "max": 2}})
                self.close(4406, "protocol incompatible")
                return
            offered = ["bridge_session", "chat_history", "command_descriptors", "device_chat",
                       "scheduled_runs", "session_list", "structured_progress"]
            self.send({"type": "HELLO_ACK", "version": 1,
                       "capabilities": sorted(set(offered) & set(hello.get("capabilities", []))),
                       "registration": {"id": "7d1e", "name": "thistle-sim", "state": "active",
                                        "conversations": {"device_chat": CONVERSATION}},
                       "session_id": "ab12", "timezone": "CET-1CEST,M3.5.0,M10.5.0/3"})
            self.sync(HISTORY)
            if bridge.mode == "busy":
                # A turn already in flight, as one started before the app opened.
                self.send({"type": "CHAT_ACTIVITY", "visibility": "all",
                           "activity": {"id": "tool-1", "label": "Reading prices",
                                        "detail": "nordpool today", "state": "running"}})
            threading.Thread(target=self.pinger, daemon=True).start()
            if bridge.mode == "drop" and bridge.connections == 1:
                time.sleep(1.0)
                self.close(1011, "session failed")
                return
            while True:
                opcode, payload = self.read_frame()
                if opcode == 0x1:
                    message = json.loads(payload)
                    bridge.received.append(message)
                    self.handle(message)
                elif opcode == 0xA:
                    bridge.pongs += 1
                elif opcode == 0x8:
                    bridge.close_codes.append(struct.unpack(">H", payload[:2])[0]
                                              if len(payload) >= 2 else 1005)
                    self.close(1000)
                    return
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            with self.lock:
                self.closed = True
            self.sock.close()


class FakeBridge:
    def __init__(self, mode: str = "normal") -> None:
        self.mode = mode
        self.received: list[dict] = []
        self.request_lines: list[str] = []
        self.connections = 0
        self.pings = 0
        self.pongs = 0
        self.unmasked = 0
        self.close_codes: list[int] = []
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)

    @property
    def url(self) -> str:
        return f"ws://127.0.0.1:{self.port}"

    def types(self) -> list[str]:
        return [m.get("type", "") for m in self.received]

    def serve(self) -> None:
        while True:
            try:
                sock, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=Connection(self, sock).run, daemon=True).start()

    def start(self) -> "FakeBridge":
        self.thread.start()
        return self

    def stop(self) -> None:
        self.server.close()
