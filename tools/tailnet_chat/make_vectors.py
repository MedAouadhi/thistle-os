#!/usr/bin/env python3
"""Write Tailnet chat's test vectors, docs/tailnet-chat/vectors/*.json.

    python3 tools/tailnet_chat/make_vectors.py           # rewrite them
    python3 tools/tailnet_chat/make_vectors.py --check   # exit 1 if stale

Everything is fixed: the seeds and nonces are SHA-256 of a label, Ed25519
signatures are deterministic, and voice.opus is committed as it is (its
recipe is in docs/tailnet-chat/vectors/README.md); only the JSON is derived.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from tailnet_chat import ed25519, ogg, protocol
else:
    from . import ed25519, ogg, protocol

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIR = os.path.join(HERE, "..", "..", "docs", "tailnet-chat", "vectors")


def fixed(label: str) -> bytes:
    return hashlib.sha256(("tailnet-chat vector " + label).encode()).digest()


def mid(label: str) -> str:
    return fixed(label)[:16].hex()


def dump(value) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def handshake() -> dict:
    sides = {}
    for role, name in (("dialler", "laptop"), ("listener", "T-Deck Max")):
        seed = fixed(role + " seed")
        key = ed25519.public_key(seed)
        nonce = fixed(role + " nonce")
        sides[role] = {"seed": seed.hex(), "key": key.hex(), "name": name,
                       "fingerprint": protocol.fingerprint(key), "nonce": nonce.hex(),
                       "hello": protocol.encode(protocol.hello(key, name, protocol.CAPS_V1, nonce))}
    d, l = sides["dialler"], sides["listener"]
    fields = [bytes.fromhex(x) for x in (d["nonce"], l["nonce"], d["key"], l["key"])]
    transcripts = {}
    for role in ("dialler", "listener"):
        t = protocol.transcript(role, *fields)
        transcripts[role] = t.hex()
        sides[role]["auth"] = protocol.encode(protocol.auth(ed25519.sign(bytes.fromhex(sides[role]["seed"]), t)))
    d_seed = bytes.fromhex(d["seed"])
    listener_t = bytes.fromhex(transcripts["listener"])
    swapped = protocol.transcript("dialler", fields[1], fields[0], fields[2], fields[3])
    stranger = fixed("stranger seed")
    refused = [
        {"why": "the dialler signed the listener's transcript (the wrong role byte)",
         "auth": protocol.encode(protocol.auth(ed25519.sign(d_seed, listener_t)))},
        {"why": "the nonces in the wrong order",
         "auth": protocol.encode(protocol.auth(ed25519.sign(d_seed, swapped)))},
        {"why": "signed by another key than the hello announced",
         "auth": protocol.encode(protocol.auth(ed25519.sign(stranger, bytes.fromhex(transcripts["dialler"]))))},
    ]
    return {"description": "A dialler and a listener's hello and auth. Each signs its own "
                           "transcript; each verifies the other's with the key in its hello. "
                           "'refused' holds dialler auths a listener MUST refuse.",
            "dialler": d, "listener": l, "transcripts": transcripts, "refused": refused}


def messages() -> dict:
    m1, m2, r1 = mid("message 1"), mid("message 2"), mid("reaction 1")
    call = mid("call 1")
    ts = 1793541600000  # 2026-11-01 14:00:00 UTC
    valid = [
        ("msg", {"type": "msg", "id": m1, "ts": ts, "text": "Are you home? مرحبا 👋"}),
        ("msg replying", {"type": "msg", "id": m2, "ts": ts + 5000, "text": "Yes", "reply_to": m1}),
        ("ack", {"type": "ack", "id": m1}),
        ("read", {"type": "read", "ids": [m1, m2]}),
        ("typing on", {"type": "typing", "on": True}),
        ("typing off", {"type": "typing", "on": False}),
        ("react", {"type": "react", "id": r1, "ts": ts + 9000, "to": m1, "emoji": "👍"}),
        ("react removed", {"type": "react", "id": mid("reaction 2"), "ts": ts + 12000, "to": m1, "emoji": ""}),
        ("contact accepted", {"type": "contact", "id": mid("contact 1"), "state": "accepted"}),
        ("contact declined", {"type": "contact", "id": mid("contact 2"), "state": "declined"}),
        ("voice", {"type": "voice", "id": mid("voice 1"), "ts": ts, "ms": 1500, "bytes": 6013,
                   "codec": "ogg-opus", "stream": 1}),
        ("call_offer", {"type": "call_offer", "call": call, "transports": ["ws-opus"], "stream": 2}),
        ("call_answer", {"type": "call_answer", "call": call, "transport": "ws-opus"}),
        ("call_answer busy", {"type": "call_answer", "call": call, "decline": "busy"}),
        ("call_end", {"type": "call_end", "call": call, "reason": "hangup"}),
        ("call_end cellular", {"type": "call_end", "call": call, "reason": "cellular"}),
        ("peers_ask", {"type": "peers_ask"}),
        ("peers", {"type": "peers", "peers": [
            {"name": "thistle-46cf20", "addrs": ["100.110.81.38", "fd7a:115c:a1e0::2e38:5128"]},
            {"name": "laptop", "addrs": ["100.101.102.103"]}]}),
        ("peers none", {"type": "peers", "peers": []}),
        ("error unsupported", {"type": "error", "code": "unsupported", "what": "sticker"}),
        ("error too_large", {"type": "error", "code": "too_large", "what": "msg", "id": m1}),
        ("hello", None),
        ("auth", None),
    ]
    hs = handshake()
    out = []
    for name, frame in valid:
        if name == "hello":
            text = hs["dialler"]["hello"]
        elif name == "auth":
            text = hs["dialler"]["auth"]
        else:
            text = protocol.encode(frame)
        out.append({"name": name, "text": text})

    invalid = [
        ("unknown type", '{"type":"sticker","id":"%s"}' % m1,
         protocol.error("unsupported", what="sticker")),
        ("text over 4096 bytes", protocol.encode({"type": "msg", "id": m1, "ts": ts, "text": "é" * 2049}),
         protocol.error("too_large", what="msg", id=m1)),
        ("empty text", protocol.encode({"type": "msg", "id": m1, "ts": ts, "text": ""}),
         protocol.error("bad_request", what="msg")),
        ("id not 32 lowercase hex", protocol.encode({"type": "ack", "id": m1.upper()}),
         protocol.error("bad_request", what="ack")),
        ("not JSON", "{type: msg}", protocol.error("bad_request")),
        ("no type", '{"id":"%s"}' % m1, protocol.error("bad_request")),
        ("typing not a boolean", '{"type":"typing","on":1}', protocol.error("bad_request", what="typing")),
        ("voice in another codec", protocol.encode({"type": "voice", "id": m1, "ts": ts, "ms": 1000,
                                                    "bytes": 100, "codec": "aac", "stream": 1}),
         protocol.error("unsupported", what="codec:aac", id=m1)),
        ("voice over 2 minutes", protocol.encode({"type": "voice", "id": m1, "ts": ts, "ms": 120001,
                                                  "bytes": 100, "codec": "ogg-opus", "stream": 1}),
         protocol.error("bad_request", what="voice")),
        ("call_answer with neither", '{"type":"call_answer","call":"%s"}' % call,
         protocol.error("bad_request", what="call_answer")),
        ("stream 0", protocol.encode({"type": "call_offer", "call": call, "transports": ["ws-opus"], "stream": 0}),
         protocol.error("bad_request", what="call_offer")),
        ("peers not a list", '{"type":"peers","peers":{"name":"laptop"}}',
         protocol.error("bad_request", what="peers")),
        ("peers over 64", protocol.encode(protocol.peers(
            [{"name": "p%d" % i, "addrs": ["100.64.0.%d" % i]} for i in range(65)])),
         protocol.error("bad_request", what="peers")),
        ("a peer with no address", '{"type":"peers","peers":[{"name":"laptop","addrs":[]}]}',
         protocol.error("bad_request", what="peers")),
        ("a peer with three addresses", protocol.encode(protocol.peers(
            [{"name": "laptop", "addrs": ["100.64.0.1", "100.64.0.2", "100.64.0.3"]}])),
         protocol.error("bad_request", what="peers")),
        ("a peer name over 64 bytes", protocol.encode(protocol.peers(
            [{"name": "x" * 65, "addrs": ["100.64.0.1"]}])),
         protocol.error("bad_request", what="peers")),
    ]
    return {"description": "One of each JSON frame, as the reference chat peer writes it "
                           "(compact, UTF-8, not ASCII-escaped). A receiver MUST accept any valid "
                           "JSON for the same object. 'invalid' frames MUST draw the error shown "
                           "(code, and 'what' and 'id' where given), and leave the connection open.",
            "valid": out,
            "invalid": [{"name": n, "text": t, "error": e} for n, t, e in invalid]}


def frames() -> dict:
    valid = [
        (protocol.KIND_VOICE, 1, 0, b"OggS"),
        (protocol.KIND_VOICE, 1, 1, b""),
        (protocol.KIND_CALL_AUDIO, 2, 0x01020304, bytes.fromhex("0b4142")),
        (protocol.KIND_CALL_AUDIO, 0xFFFFFFFF, 0xFFFFFFFF, b"\x00"),
    ]
    return {"description": "Binary frames: kind (1 byte), stream (u32 big-endian), "
                           "sequence (u32 big-endian), payload (0 to 4096 bytes).",
            "valid": [{"kind": k, "stream": s, "seq": q, "payload": p.hex(),
                       "frame": protocol.pack_binary(k, s, q, p).hex()} for k, s, q, p in valid],
            "invalid": [
                {"why": "shorter than the 9-byte header", "frame": "0100000001000000"},
                {"why": "payload of 4097 bytes", "frame": "01" + "00000001" + "00000000" + "00" * 4097},
            ]}


def voice(directory: str) -> dict:
    with open(os.path.join(directory, "voice.opus"), "rb") as f:
        data = f.read()
    opus = ogg.read_opus(data)
    stream = 1
    message = {"type": "voice", "id": mid("voice 1"), "ts": 1793541600000, "ms": opus.duration_ms,
               "bytes": len(data), "codec": "ogg-opus", "stream": stream}
    chunks = []
    for seq, at in enumerate(range(0, len(data), protocol.MAX_CHUNK)):
        frame = protocol.pack_binary(protocol.KIND_VOICE, stream, seq, data[at:at + protocol.MAX_CHUNK])
        chunks.append({"seq": seq, "bytes": len(frame) - 9, "frame": frame.hex()})
    call = [protocol.pack_binary(protocol.KIND_CALL_AUDIO, 2, seq, packet).hex()
            for seq, packet in enumerate(opus.frames[:3])]
    return {"description": "voice.opus as a voice message: the 'voice' frame, then the file in "
                           "binary chunks of at most 4096 bytes on its stream. 'frames' are its "
                           "Opus packets, 20 ms each, in order; 'call_audio' sends the first three "
                           "as a call's binary frames on stream 2.",
            "file": "voice.opus", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "channels": opus.channels, "input_rate": opus.input_rate, "pre_skip": opus.pre_skip,
            "ms": opus.duration_ms, "message": protocol.encode(message), "chunks": chunks,
            "frames": [{"seq": i, "bytes": len(p), "hex": p.hex()} for i, p in enumerate(opus.frames)],
            "call_audio": call}


CANDIDATES = [
    ("candidate:1 1 udp 2122260223 100.91.111.54 47998 typ host generation 0", True),
    ("candidate:2 1 udp 2122262783 fd7a:115c:a1e0::3936:6f36 51234 typ host generation 0", True),
    ("candidate:3 1 udp 2122194687 192.168.2.131 40000 typ host generation 0", False),
    ("candidate:4 1 udp 2122129151 10.11.12.28 40002 typ host generation 0", False),
    ("candidate:5 1 udp 2122197247 2a02:1234::5 40004 typ host generation 0", False),
    ("candidate:6 1 udp 1686052607 81.2.3.4 40006 typ srflx raddr 192.168.2.131 rport 40000", False),
    ("candidate:7 1 tcp 1518280447 100.91.111.54 9 typ host tcptype active generation 0", False),
    ("candidate:8 1 udp 2122260223 4f0c1a2e-1b9d-4c2e.local 47998 typ host generation 0", False),
    ("candidate:9 1 udp 2122260223 100.128.0.1 47998 typ host generation 0", False),
]

# An offer as libwebrtc writes one, cut short: one audio and one video
# section, bundled, with one candidate in the tailnet and one on the LAN.
SDP_OFFER = (
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=audio 47998 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 100.91.111.54\r\n"
    "a=candidate:1 1 udp 2122260223 100.91.111.54 47998 typ host generation 0\r\n"
    "a=candidate:3 1 udp 2122194687 192.168.2.131 40000 typ host generation 0\r\n"
    "a=ice-ufrag:8hhY\r\n"
    "a=ice-pwd:asd88fgpdd777uzjYhagZg5Q\r\n"
    "a=fingerprint:sha-256 19:E2:1C:3B:4B:9F:81:E6:B8:5C:F4:A5:A8:D8:73:04:BB:05:2F:70:9F:04:A9:0E:05:E9:26:33:E8:70:88:A2\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
)


# Which transports a call offer lists, by both hellos' call capabilities
# (protocol sections 4.8 and 4.11), written out by hand: a laptop, a phone
# (whose WebRTC gathers no tailnet candidate), a phone from before
# `call:webrtc-prflx`, and the T-Deck.
LAPTOP = ["call:ws-opus", "call:webrtc"]
PHONE = ["call:ws-opus", "call:webrtc", "call:webrtc-prflx"]
OLDER_PHONE = ["call:ws-opus", "call:webrtc"]
TDECK = ["call:ws-opus"]
OFFERS = [
    ("a laptop calls a phone", LAPTOP, PHONE, ["webrtc", "ws-opus"]),
    ("a phone calls a laptop", PHONE, LAPTOP, ["webrtc", "ws-opus"]),
    ("a phone calls a phone", PHONE, PHONE, ["ws-opus"]),
    ("an older phone calls a phone", OLDER_PHONE, PHONE, ["webrtc", "ws-opus"]),
    ("a phone calls an older phone", PHONE, OLDER_PHONE, ["webrtc", "ws-opus"]),
    ("a phone calls the T-Deck", PHONE, TDECK, ["ws-opus"]),
    ("call:webrtc-prflx alone is nothing of WebRTC", ["call:ws-opus", "call:webrtc-prflx"], LAPTOP, ["ws-opus"]),
]


def webrtc() -> dict:
    """WebRTC calls between video-capable chat peers (protocol section 4.11):
    their frames, which candidates are admitted, an SDP with those of the
    tailnet alone, and which transports an offer lists."""
    call = mid("call 2")
    tailnet = protocol.tailnet_only_sdp(SDP_OFFER)
    valid = [
        ("call_offer webrtc first, camera on", {"type": "call_offer", "call": call,
                                                 "transports": ["webrtc", "ws-opus"], "stream": 3, "video": True}),
        ("call_offer webrtc, camera off", {"type": "call_offer", "call": call,
                                           "transports": ["webrtc", "ws-opus"], "stream": 3, "video": False}),
        ("call_answer webrtc", {"type": "call_answer", "call": call, "transport": "webrtc"}),
        ("call_rtc offer", {"type": "call_rtc", "call": call, "sdp": {"type": "offer", "sdp": tailnet}}),
        ("call_rtc answer", {"type": "call_rtc", "call": call, "sdp": {"type": "answer", "sdp": tailnet}}),
        ("call_rtc candidate", {"type": "call_rtc", "call": call, "candidate": {
            "candidate": CANDIDATES[0][0], "sdpMid": "0", "sdpMLineIndex": 0}}),
        ("call_rtc candidate IPv6", {"type": "call_rtc", "call": call, "candidate": {
            "candidate": CANDIDATES[1][0], "sdpMid": "0", "sdpMLineIndex": 0}}),
        ("call_camera on", {"type": "call_camera", "call": call, "camera": "on"}),
        ("call_camera off", {"type": "call_camera", "call": call, "camera": "off"}),
        ("call_camera paused", {"type": "call_camera", "call": call, "camera": "paused"}),
        ("call_camera none", {"type": "call_camera", "call": call, "camera": "none"}),
        ("call_end error", {"type": "call_end", "call": call, "reason": "error"}),
    ]
    invalid = [
        ("call_rtc with neither", protocol.encode({"type": "call_rtc", "call": call}),
         protocol.error("bad_request", what="call_rtc")),
        ("call_rtc with both", protocol.encode({"type": "call_rtc", "call": call,
                                                "sdp": {"type": "offer", "sdp": tailnet},
                                                "candidate": {"candidate": CANDIDATES[0][0], "sdpMid": "0"}}),
         protocol.error("bad_request", what="call_rtc")),
        ("call_rtc sdp of another type", protocol.encode({"type": "call_rtc", "call": call,
                                                          "sdp": {"type": "pranswer", "sdp": tailnet}}),
         protocol.error("bad_request", what="call_rtc")),
        ("call_rtc empty candidate", protocol.encode({"type": "call_rtc", "call": call,
                                                      "candidate": {"candidate": "", "sdpMid": "0"}}),
         protocol.error("bad_request", what="call_rtc")),
        ("call_camera of another state", protocol.encode({"type": "call_camera", "call": call, "camera": "blurred"}),
         protocol.error("bad_request", what="call_camera")),
        ("call_offer video not a boolean", protocol.encode({"type": "call_offer", "call": call,
                                                            "transports": ["webrtc", "ws-opus"], "stream": 3,
                                                            "video": "yes"}),
         protocol.error("bad_request", what="call_offer")),
    ]
    return {"description": "WebRTC calls (protocol section 4.11). 'valid': one of each frame; 'invalid': "
                           "frames that MUST draw the error shown. 'candidates': ICE candidates and "
                           "whether a side may send or take them. 'sdp': an SDP as written, and as "
                           "sent or taken, its candidates outside the tailnet removed. 'offers': the "
                           "call capabilities of a caller's and a callee's hellos, and the transports "
                           "the caller's offer lists.",
            "valid": [{"name": n, "text": protocol.encode(f)} for n, f in valid],
            "invalid": [{"name": n, "text": t, "error": e} for n, t, e in invalid],
            "candidates": [{"candidate": c, "admitted": a} for c, a in CANDIDATES],
            "sdp": {"written": SDP_OFFER, "tailnet_only": tailnet},
            "offers": [{"name": n, "caller": a, "callee": b, "transports": t} for n, a, b, t in OFFERS]}


def build(directory: str = DEFAULT_DIR) -> dict:
    v = voice(directory)
    msgs = messages()
    # The voice example in messages.json is the real file's message.
    for example in msgs["valid"]:
        if example["name"] == "voice":
            example["text"] = v["message"]
    return {"handshake.json": dump(handshake()), "messages.json": dump(msgs),
            "frames.json": dump(frames()), "voice.json": dump(v), "webrtc.json": dump(webrtc())}


def main() -> int:
    check = "--check" in sys.argv[1:]
    stale = []
    for name, content in build().items():
        path = os.path.join(DEFAULT_DIR, name)
        try:
            with open(path, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != content:
            stale.append(name)
            if not check:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(content)
    if check and stale:
        print("stale:", ", ".join(stale))
        return 1
    print(("stale: " if check else "wrote: ") + ", ".join(stale) if stale else "up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
