"""RFC 6455 WebSocket over the standard library's sockets, both ends.

Small and blocking: one thread reads a WebSocket, any thread may write.
Fragmented messages are joined; a ping is answered with a pong as it is read.
"""

from __future__ import annotations

import base64
import hashlib
import os
import socket
import struct
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 1 << 20

TEXT, BINARY, CLOSE, PING, PONG = 0x1, 0x2, 0x8, 0x9, 0xA


class Closed(Exception):
    def __init__(self, code: int = 1006, reason: str = ""):
        super().__init__(f"closed {code} {reason}".strip())
        self.code = code
        self.reason = reason


def _read_head(sock: socket.socket) -> tuple[str, dict, bytes]:
    data = b""
    while b"\r\n\r\n" not in data:
        try:
            chunk = sock.recv(1024)
        except (ConnectionResetError, BrokenPipeError):
            chunk = b""
        if not chunk:
            raise Closed(1006, "closed during the upgrade")
        data += chunk
        if len(data) > 8192:
            raise Closed(1002, "upgrade too long")
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return lines[0], headers, rest


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def server_upgrade(sock: socket.socket, path: str) -> "WebSocket":
    """Answer a client's upgrade on `path`; anything else gets a 404 and Closed."""
    request, headers, rest = _read_head(sock)
    parts = request.split(" ")
    key = headers.get("sec-websocket-key", "")
    if (len(parts) != 3 or parts[0] != "GET" or parts[1] != path or not key
            or headers.get("upgrade", "").lower() != "websocket"):
        sock.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        raise Closed(1002, "not an upgrade to " + path)
    sock.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n").encode())
    return WebSocket(sock, client=False, buffered=rest)


def client_upgrade(sock: socket.socket, host: str, port: int, path: str) -> "WebSocket":
    key = base64.b64encode(os.urandom(16)).decode()
    hostport = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    sock.sendall((
        f"GET {path} HTTP/1.1\r\nHost: {hostport}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    status, headers, rest = _read_head(sock)
    if " 101 " not in status + " " or headers.get("sec-websocket-accept") != accept_key(key):
        raise Closed(1002, "upgrade refused: " + status)
    return WebSocket(sock, client=True, buffered=rest)


class WebSocket:
    def __init__(self, sock: socket.socket, client: bool, buffered: bytes = b""):
        self.sock = sock
        self.client = client
        self.buffered = buffered
        self.lock = threading.Lock()
        self.closed = False
        self.close_code = None

    # -- writing ----------------------------------------------------------

    def _send(self, opcode: int, payload: bytes) -> None:
        head = bytes([0x80 | opcode])
        n = len(payload)
        bit = 0x80 if self.client else 0
        if n < 126:
            head += bytes([bit | n])
        elif n < 65536:
            head += bytes([bit | 126]) + struct.pack(">H", n)
        else:
            head += bytes([bit | 127]) + struct.pack(">Q", n)
        if self.client:
            mask = os.urandom(4)
            head += mask
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self.lock:
            if self.closed:
                raise Closed(self.close_code or 1006, "already closed")
            try:
                self.sock.sendall(head + payload)
            except OSError as e:
                self.closed = True
                raise Closed(1006, str(e))

    def send_text(self, text: str) -> None:
        self._send(TEXT, text.encode("utf-8"))

    def send_binary(self, data: bytes) -> None:
        self._send(BINARY, bytes(data))

    def ping(self, data: bytes = b"") -> None:
        self._send(PING, data)

    def close(self, code: int = 1000, reason: str = "") -> None:
        try:
            self._send(CLOSE, struct.pack(">H", code) + reason.encode()[:120])
        except Closed:
            pass
        with self.lock:
            self.closed = True
            if self.close_code is None:
                self.close_code = code
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass

    # -- reading ----------------------------------------------------------

    def _exact(self, n: int) -> bytes:
        data = self.buffered[:n]
        self.buffered = self.buffered[n:]
        while len(data) < n:
            try:
                chunk = self.sock.recv(max(4096, n - len(data)))
            except TimeoutError:
                # Nothing lost: what was read waits for the next call.
                self.buffered = data + self.buffered
                raise
            except OSError:
                chunk = b""
            if not chunk:
                raise Closed(1006, "connection lost")
            need = n - len(data)
            data += chunk[:need]
            self.buffered += chunk[need:]
        return data

    def _frame(self) -> tuple[bool, int, bytes]:
        b0, b1 = self._exact(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._exact(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._exact(8))[0]
        if n > MAX_MESSAGE:
            raise Closed(1009, "frame too big")
        masked = bool(b1 & 0x80)
        mask = self._exact(4) if masked else None
        payload = self._exact(n)
        if mask:
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        return bool(b0 & 0x80), b0 & 0x0F, payload

    def recv(self) -> tuple[int, bytes]:
        """The next message, TEXT or BINARY, and its payload; Closed at the end."""
        message, opcode = b"", None
        while True:
            fin, op, payload = self._frame()
            if op == PING:
                try:
                    self._send(PONG, payload)
                except Closed:
                    pass
                continue
            if op == PONG:
                continue
            if op == CLOSE:
                code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 1005
                reason = payload[2:].decode("utf-8", "replace")
                with self.lock:
                    already = self.closed
                    self.close_code = self.close_code or code
                if not already:
                    self.close(code)
                raise Closed(code, reason)
            if op in (TEXT, BINARY):
                opcode, message = op, payload
            elif op == 0 and opcode is not None:
                message += payload
            else:
                raise Closed(1002, f"unexpected opcode {op}")
            if len(message) > MAX_MESSAGE:
                raise Closed(1009, "message too big")
            if fin:
                return opcode, message
