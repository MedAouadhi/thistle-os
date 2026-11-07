"""Tailnet chat, protocol version 1: the parts with no I/O.

docs/tailnet-chat/protocol.md is the authority; this module is its reference
reading. Everything here is pure: the handshake's transcript, the key
fingerprint, the binary frame header, building and checking JSON frames.
"""

from __future__ import annotations

import hashlib
import ipaddress
import json
import struct

VERSION = 1
PORT = 4810
PATH = "/tailnet-chat/1"

CAPS_V1 = ("text", "receipts", "typing", "reply", "react", "voice-message", "call:ws-opus", "peers")
# A video-capable chat peer adds it (section 4.11); the T-Deck does not.
CAP_WEBRTC = "call:webrtc"
# And one whose WebRTC gathers no tailnet candidate (Android's) adds this
# too: two such peers call on ws-opus (section 4.11).
CAP_WEBRTC_PRFLX = "call:webrtc-prflx"
CAMERA_STATES = ("on", "off", "paused", "none")

LABEL = b"tailnet-chat/1 auth\x00"
ROLES = {"dialler": 1, "listener": 2}

KIND_VOICE = 0x01
KIND_CALL_AUDIO = 0x02
KIND_FILE = 0x03  # reserved for the `file` message; no v1 peer sends it
KINDS = (KIND_VOICE, KIND_CALL_AUDIO)
HEADER = struct.Struct(">BII")
MAX_CHUNK = 4096

MAX_TEXT_BYTES = 4096
MAX_JSON_FRAME = 16384
MAX_VOICE_MS = 120_000
MAX_VOICE_BYTES = 512 * 1024
MAX_NAME_BYTES = 64
MAX_PEERS = 64
MAX_PEER_ADDRS = 2

# WebSocket close codes (RFC 6455 leaves 4000-4999 to applications).
CLOSE_HANDSHAKE = 4000
CLOSE_DUPLICATE = 4001
CLOSE_KEY_CHANGED = 4002
CLOSE_IDLE = 1000

TAILNET_RANGES = (ipaddress.ip_network("100.64.0.0/10"),
                  ipaddress.ip_network("fd7a:115c:a1e0::/48"))


def _check(name: str, value: bytes, size: int) -> None:
    if not isinstance(value, (bytes, bytearray)) or len(value) != size:
        raise ValueError(f"{name} must be {size} bytes")


def transcript(role: str, dialler_nonce: bytes, listener_nonce: bytes,
               dialler_key: bytes, listener_key: bytes) -> bytes:
    """What `role` ("dialler" or "listener") signs in its `auth`."""
    if role not in ROLES:
        raise ValueError(f"no role {role!r}")
    _check("dialler_nonce", dialler_nonce, 32)
    _check("listener_nonce", listener_nonce, 32)
    _check("dialler_key", dialler_key, 32)
    _check("listener_key", listener_key, 32)
    return (LABEL + bytes([ROLES[role]]) + bytes(dialler_nonce) + bytes(listener_nonce)
            + bytes(dialler_key) + bytes(listener_key))


def fingerprint_id(key: bytes) -> str:
    """The key's fingerprint as one word: a file name, a log line."""
    _check("key", key, 32)
    return hashlib.sha256(key).hexdigest()[:20]


def fingerprint(key: bytes) -> str:
    """The key's fingerprint as a person compares it: five groups of four."""
    word = fingerprint_id(key)
    return " ".join(word[i:i + 4] for i in range(0, 20, 4))


def pack_binary(kind: int, stream: int, seq: int, payload: bytes) -> bytes:
    if len(payload) > MAX_CHUNK:
        raise ValueError("a binary frame's payload is at most 4096 bytes")
    return HEADER.pack(kind, stream, seq) + bytes(payload)


def unpack_binary(frame: bytes) -> tuple[int, int, int, bytes]:
    if len(frame) < HEADER.size:
        raise ValueError("a binary frame is at least its 9-byte header")
    if len(frame) - HEADER.size > MAX_CHUNK:
        raise ValueError("a binary frame's payload is at most 4096 bytes")
    kind, stream, seq = HEADER.unpack_from(frame)
    return kind, stream, seq, bytes(frame[HEADER.size:])


def admitted(address: str, loopback: bool = False) -> bool:
    """Whether a connection from `address` may reach the handshake."""
    ip = _ip(address)
    if ip is None:
        return False
    if loopback and ip.is_loopback:
        return True
    return any(ip in net for net in TAILNET_RANGES)


def _ip(address):
    """`address` as an IP address, an IPv4-mapped IPv6 one as its IPv4; None if it is not one."""
    try:
        ip = ipaddress.ip_address(str(address).split("%", 1)[0])
    except ValueError:
        return None
    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    return ip


def in_tailnet(address) -> bool:
    ip = _ip(address)
    return ip is not None and any(ip in net for net in TAILNET_RANGES)


def _cut(text: str, max_bytes: int) -> str:
    data = text.encode("utf-8")[:max_bytes]
    return data.decode("utf-8", "ignore")


def shared_peers(online, asker) -> list:
    """What a `peers` frame answers a contact (protocol section 4.10): the
    tailnet peers the platform shows online, `{"name", "addrs"}` each, less
    the one at the asker's address, each with at most two of its addresses
    inside the tailnet ranges, a name of at most 64 bytes, and at most 64."""
    mine = _ip(asker) if asker else None
    out = []
    for peer in online:
        ips = [_ip(a) for a in peer.get("addrs", [])]
        if mine is not None and mine in ips:
            continue
        addrs = [str(ip) for ip in ips if ip is not None and in_tailnet(ip)][:MAX_PEER_ADDRS]
        name = _cut(peer.get("name") or "", MAX_NAME_BYTES)
        if addrs and name:
            out.append({"name": name, "addrs": addrs})
        if len(out) == MAX_PEERS:
            break
    return out


def addresses_to_probe(peers) -> list:
    """The addresses an asker may probe from a `peers` frame's list: those
    inside the tailnet ranges, each once. Nothing else of the list is taken."""
    out = []
    for peer in peers:
        for a in peer.get("addrs", []):
            ip = _ip(a)
            if ip is not None and in_tailnet(ip) and str(ip) not in out:
                out.append(str(ip))
    return out


def webrtc_between(mine, theirs) -> bool:
    """Whether a call between two hellos' peers runs on WebRTC: both announce
    `call:webrtc`, and not both `call:webrtc-prflx` (no candidate between
    them)."""
    return (CAP_WEBRTC in mine and CAP_WEBRTC in theirs
            and not (CAP_WEBRTC_PRFLX in mine and CAP_WEBRTC_PRFLX in theirs))


def offer_transports(mine, theirs) -> list:
    """What a call offer lists, best first: `webrtc` when both hellos allow it
    (webrtc_between), then `ws-opus` when both announce `call:ws-opus`."""
    out = []
    if webrtc_between(mine, theirs):
        out.append("webrtc")
    if "call:ws-opus" in mine and "call:ws-opus" in theirs:
        out.append("ws-opus")
    return out


def pick_transport(offered, caps) -> str | None:
    """The answer's pick: `webrtc` whenever offered and announced, else `ws-opus`."""
    if "webrtc" in offered and CAP_WEBRTC in caps:
        return "webrtc"
    if "ws-opus" in offered and "call:ws-opus" in caps:
        return "ws-opus"
    return None


def candidate_admitted(candidate: str) -> bool:
    """Whether an ICE candidate may be sent or taken (section 4.11): UDP, from
    an address in the tailnet's ranges; with or without SDP's `a=`."""
    line = str(candidate).strip()
    if line.startswith("a="):
        line = line[2:]
    if not line.startswith("candidate:"):
        return False
    f = line.split()
    if len(f) < 8 or f[2].lower() != "udp":
        return False
    return in_tailnet(f[4])


def tailnet_only_sdp(sdp: str) -> str:
    """An SDP with each `a=candidate` line outside the tailnet taken out."""
    return "".join(line for line in sdp.splitlines(keepends=True)
                   if not line.startswith("a=candidate:") or candidate_admitted(line))


def encode(frame: dict) -> str:
    """A JSON frame as the reference peer and the vectors write it.

    Receivers MUST accept any valid JSON (key order, spacing, escapes);
    this compact form is only what makes the vectors byte-exact."""
    return json.dumps(frame, separators=(",", ":"), ensure_ascii=False)


def hello(key: bytes, name: str, caps, nonce: bytes, version: int = VERSION) -> dict:
    return {"type": "hello", "version": version, "key": bytes(key).hex(), "name": name,
            "caps": list(caps), "nonce": bytes(nonce).hex()}


def peers_ask() -> dict:
    return {"type": "peers_ask"}


def peers(shared) -> dict:
    return {"type": "peers", "peers": list(shared)}


def auth(signature: bytes) -> dict:
    return {"type": "auth", "sig": bytes(signature).hex()}


# -- checking a JSON frame -----------------------------------------------------

def _hex(n):
    def ok(v):
        return (isinstance(v, str) and len(v) == n
                and all(c in "0123456789abcdef" for c in v))
    return ok


_ID = _hex(32)
_KEY = _hex(64)
_SIG = _hex(128)


def _int(lo, hi):
    return lambda v: isinstance(v, int) and not isinstance(v, bool) and lo <= v <= hi


def _str(max_bytes, empty=True):
    return lambda v: (isinstance(v, str) and (empty or v != "")
                      and len(v.encode("utf-8")) <= max_bytes)


def _one_of(*values):
    return lambda v: v in values


def _list(item, lo, hi):
    return lambda v: isinstance(v, list) and lo <= len(v) <= hi and all(item(x) for x in v)


def _peer(v):
    return (isinstance(v, dict) and _str(MAX_NAME_BYTES, empty=False)(v.get("name"))
            and _list(_str(64, empty=False), 1, MAX_PEER_ADDRS)(v.get("addrs")))


def _sdp(v):
    return (isinstance(v, dict) and v.get("type") in ("offer", "answer")
            and _str(MAX_JSON_FRAME, empty=False)(v.get("sdp")))


def _candidate(v):
    return (isinstance(v, dict) and _str(1024, empty=False)(v.get("candidate"))
            and ("sdpMid" not in v or v["sdpMid"] is None or _str(64)(v["sdpMid"]))
            and ("sdpMLineIndex" not in v or v["sdpMLineIndex"] is None or _int(0, 65535)(v["sdpMLineIndex"])))


_BOOL = lambda v: isinstance(v, bool)  # noqa: E731
_U32 = _int(1, 0xFFFFFFFF)
_TS = _int(0, 2 ** 53)

# type -> (required fields, optional fields). Fields not listed are ignored.
SCHEMA = {
    "hello": ({"version": _int(0, 2 ** 31), "key": _KEY, "name": _str(MAX_NAME_BYTES),
               "caps": _list(_str(64, empty=False), 0, 64), "nonce": _KEY}, {}),
    "auth": ({"sig": _SIG}, {}),
    "msg": ({"id": _ID, "ts": _TS, "text": lambda v: isinstance(v, str)},
            {"reply_to": _ID}),
    "ack": ({"id": _ID}, {}),
    "read": ({"ids": _list(_ID, 1, 100)}, {}),
    "typing": ({"on": _BOOL}, {}),
    "react": ({"id": _ID, "ts": _TS, "to": _ID, "emoji": _str(32)}, {}),
    "contact": ({"id": _ID, "state": _one_of("accepted", "declined")}, {}),
    "voice": ({"id": _ID, "ts": _TS, "ms": _int(1, MAX_VOICE_MS),
               "bytes": _int(1, MAX_VOICE_BYTES), "codec": _str(32, empty=False),
               "stream": _U32}, {"reply_to": _ID}),
    "call_offer": ({"call": _ID, "transports": _list(_str(32, empty=False), 1, 8),
                    "stream": _U32}, {"video": _BOOL}),
    "call_answer": ({"call": _ID}, {"transport": _str(32, empty=False),
                                    "decline": _str(32, empty=False)}),
    "call_end": ({"call": _ID, "reason": _str(32, empty=False)}, {}),
    "call_rtc": ({"call": _ID}, {"sdp": _sdp, "candidate": _candidate}),
    "call_camera": ({"call": _ID, "camera": _one_of(*CAMERA_STATES)}, {}),
    "peers_ask": ({}, {}),
    "peers": ({"peers": _list(_peer, 0, MAX_PEERS)}, {}),
    "error": ({"code": _str(32, empty=False)},
              {"what": _str(256), "id": _ID, "detail": _str(512)}),
}

ERROR_CODES = ("unsupported", "bad_request", "too_large", "version", "bad_signature",
               "key_changed")


def error(code: str, what: str | None = None, id: str | None = None,
          detail: str | None = None) -> dict:
    frame = {"type": "error", "code": code}
    if what is not None:
        frame["what"] = what
    if id is not None:
        frame["id"] = id
    if detail is not None:
        frame["detail"] = detail
    return frame


def check(text) -> tuple[dict | None, dict | None]:
    """Parse and check one JSON text frame.

    Returns (frame, None) when it is well formed, or (None, error frame) for
    the receiver to send back: `unsupported` for a type or codec it does not
    know, `too_large` for text over 4096 bytes or a frame over 16 KB,
    `bad_request` for anything else malformed."""
    if isinstance(text, (bytes, bytearray)):
        text = bytes(text).decode("utf-8", "replace")
    if len(text.encode("utf-8")) > MAX_JSON_FRAME:
        return None, error("too_large", detail="frame over 16384 bytes")
    try:
        frame = json.loads(text)
    except ValueError:
        return None, error("bad_request", detail="not JSON")
    if not isinstance(frame, dict) or not isinstance(frame.get("type"), str):
        return None, error("bad_request", detail="not an object with a type")
    kind = frame["type"]
    if kind not in SCHEMA:
        return None, error("unsupported", what=kind[:256])
    required, optional = SCHEMA[kind]
    for name, ok in required.items():
        if name not in frame or not ok(frame[name]):
            return None, error("bad_request", what=kind, detail=f"field {name}")
    for name, ok in optional.items():
        if name in frame and not ok(frame[name]):
            return None, error("bad_request", what=kind, detail=f"field {name}")
    if kind == "msg" and len(frame["text"].encode("utf-8")) > MAX_TEXT_BYTES:
        return None, error("too_large", what="msg", id=frame["id"])
    if kind == "msg" and frame["text"] == "":
        return None, error("bad_request", what="msg", detail="empty text")
    if kind == "voice" and frame["codec"] != "ogg-opus":
        return None, error("unsupported", what="codec:" + frame["codec"], id=frame["id"])
    if kind == "call_answer" and ("transport" in frame) == ("decline" in frame):
        return None, error("bad_request", what=kind, detail="transport or decline")
    if kind == "call_rtc" and ("sdp" in frame) == ("candidate" in frame):
        return None, error("bad_request", what=kind, detail="sdp or candidate")
    return frame, None
