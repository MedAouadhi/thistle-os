#!/usr/bin/env python3
"""Tailnet chat's reference chat peer, checked against its protocol's test
vectors (docs/tailnet-chat/vectors/) and run two instances at a time.

    python3 tools/test_tailnet_chat.py
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tailnet_chat import conformance, ed25519, make_vectors, ogg, protocol  # noqa: E402
from tailnet_chat.peer import ChatPeer, Refused  # noqa: E402


class Ed25519(unittest.TestCase):
    # RFC 8032, section 7.1, tests 1 and 2.
    RFC = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ]

    def test_pure_python_matches_rfc8032(self):
        for seed, public, msg, sig in self.RFC:
            seed, msg = bytes.fromhex(seed), bytes.fromhex(msg)
            self.assertEqual(ed25519.pure_public_key(seed).hex(), public)
            self.assertEqual(ed25519.pure_sign(seed, msg).hex(), sig)
            self.assertTrue(ed25519.pure_verify(bytes.fromhex(public), msg, bytes.fromhex(sig)))
            bad = bytearray(bytes.fromhex(sig))
            bad[0] ^= 1
            self.assertFalse(ed25519.pure_verify(bytes.fromhex(public), msg, bytes(bad)))

    def test_the_fast_path_agrees(self):
        for seed, public, msg, sig in self.RFC:
            seed, msg = bytes.fromhex(seed), bytes.fromhex(msg)
            self.assertEqual(ed25519.public_key(seed).hex(), public)
            self.assertEqual(ed25519.sign(seed, msg).hex(), sig)
            self.assertTrue(ed25519.verify(bytes.fromhex(public), msg, bytes.fromhex(sig)))
            self.assertFalse(ed25519.verify(bytes.fromhex(public), msg + b"x", bytes.fromhex(sig)))


class Handshake(unittest.TestCase):
    D_KEY = bytes(range(32))
    L_KEY = bytes(range(32, 64))
    D_NONCE = b"\x11" * 32
    L_NONCE = b"\x22" * 32

    def test_the_transcript_is_label_role_nonces_keys(self):
        label = b"tailnet-chat/1 auth\x00"
        for role, byte in (("dialler", b"\x01"), ("listener", b"\x02")):
            got = protocol.transcript(role, self.D_NONCE, self.L_NONCE, self.D_KEY, self.L_KEY)
            self.assertEqual(got, label + byte + self.D_NONCE + self.L_NONCE + self.D_KEY + self.L_KEY)
            self.assertEqual(len(got), 149)

    def test_a_transcript_refuses_short_fields(self):
        with self.assertRaises(ValueError):
            protocol.transcript("dialler", b"\x11" * 31, self.L_NONCE, self.D_KEY, self.L_KEY)
        with self.assertRaises(ValueError):
            protocol.transcript("caller", self.D_NONCE, self.L_NONCE, self.D_KEY, self.L_KEY)

    def test_the_fingerprint_is_sha256s_first_ten_bytes_in_fours(self):
        # SHA-256 of 00 01 .. 1f is 630dcd29 66c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd
        self.assertEqual(protocol.fingerprint(self.D_KEY), "630d cd29 66c4 3366 9112")
        self.assertEqual(protocol.fingerprint_id(self.D_KEY), "630dcd2966c433669112")


class BinaryFrames(unittest.TestCase):
    def test_the_header_is_kind_stream_sequence_big_endian(self):
        frame = protocol.pack_binary(protocol.KIND_VOICE, 0x01020304, 7, b"ab")
        self.assertEqual(frame, bytes.fromhex("01 01020304 00000007") + b"ab")
        self.assertEqual(protocol.unpack_binary(frame), (1, 0x01020304, 7, b"ab"))

    def test_a_frame_shorter_than_its_header_is_refused(self):
        with self.assertRaises(ValueError):
            protocol.unpack_binary(b"\x01\x00\x00\x00\x01\x00\x00")

    def test_a_payload_over_4_kb_is_refused(self):
        with self.assertRaises(ValueError):
            protocol.pack_binary(protocol.KIND_VOICE, 1, 0, b"\0" * 4097)


ID = "0123456789abcdef0123456789abcdef"


class Checking(unittest.TestCase):
    def test_a_well_formed_message_passes(self):
        frame, error = protocol.check('{"type":"msg","id":"%s","ts":1,"text":"hi"}' % ID)
        self.assertIsNone(error)
        self.assertEqual(frame["text"], "hi")

    def test_fields_it_does_not_know_are_ignored(self):
        _, error = protocol.check('{"type":"ack","id":"%s","later":true}' % ID)
        self.assertIsNone(error)

    def test_an_unknown_type_is_unsupported_naming_it(self):
        _, error = protocol.check('{"type":"sticker","id":"%s"}' % ID)
        self.assertEqual(error, {"type": "error", "code": "unsupported", "what": "sticker"})

    def test_text_over_4096_bytes_is_too_large(self):
        text = "é" * 2049  # 4098 bytes
        _, error = protocol.check(protocol.encode({"type": "msg", "id": ID, "ts": 1, "text": text}))
        self.assertEqual(error["code"], "too_large")
        self.assertEqual(error["id"], ID)

    def test_4096_bytes_of_text_is_allowed(self):
        _, error = protocol.check(protocol.encode({"type": "msg", "id": ID, "ts": 1, "text": "é" * 2048}))
        self.assertIsNone(error)

    def test_malformed_frames_are_bad_requests(self):
        for text in ('not json', '[1]', '{"id":"x"}', '{"type":7}',
                     '{"type":"msg","id":"ABC","ts":1,"text":"x"}',
                     '{"type":"msg","id":"%s","ts":-1,"text":"x"}' % ID,
                     '{"type":"typing","on":"yes"}',
                     '{"type":"contact","id":"%s","state":"maybe"}' % ID,
                     '{"type":"call_answer","call":"%s"}' % ID):
            _, error = protocol.check(text)
            self.assertEqual(error and error["code"], "bad_request", text)

    def test_a_voice_message_in_another_codec_is_unsupported(self):
        _, error = protocol.check(protocol.encode({
            "type": "voice", "id": ID, "ts": 1, "ms": 1000, "bytes": 10,
            "codec": "aac", "stream": 1}))
        self.assertEqual(error, {"type": "error", "code": "unsupported", "what": "codec:aac", "id": ID})


PEER_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tailnet_chat", "peer.py")
VECTORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "tailnet-chat", "vectors")


def opus_frame_ms(toc: int) -> float:
    # RFC 6716, 3.1: the configuration's frame size.
    config = toc >> 3
    if config < 12:
        return (10, 20, 40, 60)[config % 4]
    if config < 16:
        return (10, 20)[config % 2]
    return (2.5, 5, 10, 20)[config % 4]


class OggOpus(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(VECTORS, "voice.opus"), "rb") as f:
            self.data = f.read()

    def test_the_voice_vector_is_mono_16_khz_in_20_ms_frames(self):
        voice = ogg.read_opus(self.data)
        self.assertEqual(voice.channels, 1)
        self.assertEqual(voice.input_rate, 16000)
        self.assertGreaterEqual(len(voice.frames), 75)
        for frame in voice.frames:
            self.assertEqual(opus_frame_ms(frame[0]), 20)
            self.assertEqual(frame[0] & 0x04, 0, "mono")
            self.assertEqual(frame[0] & 0x03, 0, "one frame a packet")
        self.assertEqual(voice.duration_ms, 1500)

    def test_a_corrupt_page_is_refused(self):
        bad = bytearray(self.data)
        bad[len(bad) // 2] ^= 0xFF
        with self.assertRaises(ValueError):
            ogg.read_opus(bytes(bad))


def load(name):
    with open(os.path.join(VECTORS, name), encoding="utf-8") as f:
        return json.load(f)


class Vectors(unittest.TestCase):
    def test_the_committed_vectors_are_what_the_generator_writes(self):
        for name, content in make_vectors.build(VECTORS).items():
            with open(os.path.join(VECTORS, name), encoding="utf-8") as f:
                self.assertEqual(f.read(), content, f"{name} is stale: run tools/tailnet_chat/make_vectors.py")

    def test_the_handshake_vector_holds_together(self):
        v = load("handshake.json")
        d, l = v["dialler"], v["listener"]
        for side in (d, l):
            seed = bytes.fromhex(side["seed"])
            self.assertEqual(ed25519.pure_public_key(seed).hex(), side["key"])
            hello, error = protocol.check(side["hello"])
            self.assertIsNone(error)
            self.assertEqual((hello["key"], hello["nonce"], hello["version"]), (side["key"], side["nonce"], 1))
            self.assertEqual(protocol.fingerprint(bytes.fromhex(side["key"])), side["fingerprint"])
        nonces_keys = bytes.fromhex(d["nonce"] + l["nonce"] + d["key"] + l["key"])
        for role, side in (("dialler", d), ("listener", l)):
            role_byte = {"dialler": b"\x01", "listener": b"\x02"}[role]
            self.assertEqual(v["transcripts"][role], (b"tailnet-chat/1 auth\x00" + role_byte + nonces_keys).hex())
            auth, error = protocol.check(side["auth"])
            self.assertIsNone(error)
            self.assertTrue(ed25519.pure_verify(bytes.fromhex(side["key"]),
                                                bytes.fromhex(v["transcripts"][role]),
                                                bytes.fromhex(auth["sig"])))
        for bad in v["refused"]:
            auth, _ = protocol.check(bad["auth"])
            self.assertFalse(ed25519.pure_verify(bytes.fromhex(d["key"]),
                                                 bytes.fromhex(v["transcripts"]["dialler"]),
                                                 bytes.fromhex(auth["sig"])), bad["why"])

    def test_every_message_type_has_a_valid_example(self):
        v = load("messages.json")
        seen = set()
        for example in v["valid"]:
            frame, error = protocol.check(example["text"])
            self.assertIsNone(error, example["name"])
            seen.add(frame["type"])
        self.assertEqual(seen, set(protocol.SCHEMA))

    def test_every_invalid_example_draws_its_error(self):
        for example in load("messages.json")["invalid"]:
            _, error = protocol.check(example["text"])
            self.assertEqual(error and error["code"], example["error"]["code"], example["name"])
            for name in ("what", "id"):
                if name in example["error"]:
                    self.assertEqual(error.get(name), example["error"][name], example["name"])

    def test_binary_frame_headers(self):
        for example in load("frames.json")["valid"]:
            got = protocol.unpack_binary(bytes.fromhex(example["frame"]))
            self.assertEqual(got, (example["kind"], example["stream"], example["seq"],
                                   bytes.fromhex(example["payload"])))
        for example in load("frames.json")["invalid"]:
            with self.assertRaises(ValueError, msg=example["why"]):
                protocol.unpack_binary(bytes.fromhex(example["frame"]))

    def test_the_voice_vector_matches_its_file(self):
        v = load("voice.json")
        with open(os.path.join(VECTORS, v["file"]), "rb") as f:
            data = f.read()
        self.assertEqual(len(data), v["bytes"])
        self.assertEqual(hashlib.sha256(data).hexdigest(), v["sha256"])
        voice, error = protocol.check(v["message"])
        self.assertIsNone(error)
        self.assertEqual((voice["bytes"], voice["ms"], voice["codec"]), (len(data), 1500, "ogg-opus"))
        self.assertEqual([bytes.fromhex(f["hex"]) for f in v["frames"]], ogg.read_opus(data).frames)
        joined = b""
        for seq, chunk in enumerate(v["chunks"]):
            kind, stream, got_seq, payload = protocol.unpack_binary(bytes.fromhex(chunk["frame"]))
            self.assertEqual((kind, stream, got_seq), (protocol.KIND_VOICE, voice["stream"], seq))
            self.assertLessEqual(len(payload), 4096)
            joined += payload
        self.assertEqual(joined, data)


class TwoPeers(unittest.TestCase):
    def setUp(self):
        self.alice = ChatPeer(name="alice")
        self.bob = ChatPeer(name="bob")
        self.alice.start()
        self.bob.start()
        self.addCleanup(self.alice.stop)
        self.addCleanup(self.bob.stop)

    def test_two_peers_shake_hands_and_learn_each_others_key(self):
        conn = self.alice.connect(self.bob.address)
        self.assertEqual(conn.peer_key, self.bob.key)
        self.assertEqual(conn.peer_name, "bob")
        self.bob.wait_for(lambda: self.bob.connected(self.alice.key_hex))

    def befriend(self):
        """Alice adds Bob by address and writes; Bob accepts."""
        self.alice.add_contact(self.bob.address, name="Bob")
        mid = self.alice.send_text(self.bob.address, "hello bob")
        self.bob.wait_for(lambda: self.bob.requests.get(self.alice.key_hex))
        self.bob.accept(self.alice.key_hex)
        self.alice.wait_for(lambda: self.alice.contact("Bob").accepted_by_them)
        return mid

    def test_a_contact_request_accepted_delivers_what_it_held(self):
        mid = self.befriend()
        self.assertTrue(self.alice.delivered(mid))
        self.assertEqual(self.alice.contact("Bob").key, self.bob.key_hex, "pinned at first handshake")
        self.assertEqual([m.text for m in self.bob.messages(self.alice.key_hex)], ["hello bob"])
        self.assertEqual(self.alice.waiting("Bob"), [])

    def test_a_declined_sender_is_told_and_stops_sending(self):
        self.alice.add_contact(self.bob.address, name="Bob")
        self.alice.send_text(self.bob.address, "hello bob")
        self.bob.wait_for(lambda: self.bob.requests.get(self.alice.key_hex))
        self.bob.decline(self.alice.key_hex)
        self.alice.wait_for(lambda: self.alice.contact("Bob").declined_by_them)
        self.assertEqual(self.bob.messages(self.alice.key_hex), [])
        self.assertEqual(self.alice.waiting("Bob"), [])
        with self.assertRaises(Refused):
            self.alice.send_text("Bob", "again?")

    def test_a_reply_and_a_reaction_arrive_and_a_reaction_can_be_removed(self):
        first = self.befriend()
        reply = self.bob.send_text(self.alice.key_hex, "hi alice", reply_to=first)
        got = self.alice.wait_for(lambda: self.alice.message(reply))
        self.assertEqual(got.reply_to, first)
        self.alice.react("Bob", reply, "👍")
        self.bob.wait_for(lambda: self.bob.message(reply).reactions.get("them") == "👍")
        self.alice.react("Bob", reply, "")
        self.bob.wait_for(lambda: "them" not in self.bob.message(reply).reactions)

    def test_read_receipts_go_only_while_allowed(self):
        self.befriend()
        one = self.bob.send_text(self.alice.key_hex, "one")
        self.alice.wait_for(lambda: self.alice.message(one))
        self.alice.mark_read("Bob")
        self.bob.wait_for(lambda: self.bob.message(one).state == "read")
        self.alice.read_receipts = False
        two = self.bob.send_text(self.alice.key_hex, "two")
        self.alice.wait_for(lambda: self.alice.message(two))
        self.alice.mark_read("Bob")
        time.sleep(0.3)
        self.assertEqual(self.bob.message(two).state, "delivered")
        self.assertEqual(self.bob.received(self.alice.key_hex, "read")[-1]["ids"], [one])

    def test_typing_lapses_unless_repeated(self):
        self.befriend()
        self.alice.set_typing("Bob", True)
        self.bob.wait_for(lambda: self.bob.is_typing(self.alice.key_hex))
        self.alice.set_typing("Bob", False)
        self.bob.wait_for(lambda: not self.bob.is_typing(self.alice.key_hex))

    def test_the_outbox_waits_for_a_peer_that_is_away(self):
        self.befriend()
        port = self.bob.port
        self.bob.stop()
        mid = self.alice.send_text("Bob", "are you there?")
        time.sleep(0.3)
        self.assertEqual(self.alice.waiting("Bob"), [mid])
        bob = ChatPeer(name="bob", seed=self.bob.seed, port=port)
        bob.contacts, bob.conversations = self.bob.contacts, self.bob.conversations
        bob.start()
        self.addCleanup(bob.stop)
        self.alice.flush()
        self.alice.wait_for(lambda: self.alice.delivered(mid))
        self.assertEqual(bob.messages(self.alice.key_hex)[-1].text, "are you there?")

    def test_a_listener_with_a_bad_signature_is_refused(self):
        self.bob.sign_override = lambda transcript: bytes(64)
        with self.assertRaises(Refused):
            self.alice.connect(self.bob.address)

    def test_a_dialler_with_a_bad_signature_is_refused(self):
        self.alice.sign_override = lambda transcript: ed25519.sign(self.alice.seed, transcript[:-1])
        with self.assertRaises(Refused) as refused:
            self.alice.connect(self.bob.address)
        self.assertEqual(refused.exception.code, protocol.CLOSE_HANDSHAKE)
        self.assertEqual(refused.exception.error, "bad_signature")
        self.bob.wait_for(lambda: any(e[0] == "refused" for e in self.bob.events))
        self.assertIsNone(self.bob.connected(self.alice.key_hex))

    def test_an_unknown_type_is_answered_and_the_connection_stays(self):
        conn = self.alice.connect(self.bob.address)
        conn.send({"type": "sticker", "id": ID})
        self.alice.wait_for(lambda: self.alice.errors)
        self.assertEqual(self.alice.errors[0][1], {"type": "error", "code": "unsupported", "what": "sticker"})
        self.assertIsNotNone(self.bob.connected(self.alice.key_hex))

    def test_only_the_tailnet_is_admitted_unless_loopback_is(self):
        strict = ChatPeer(name="strict", loopback=False).start()
        self.addCleanup(strict.stop)
        with self.assertRaises(Refused):
            self.alice.connect(strict.address)
        self.assertTrue(protocol.admitted("100.101.102.103"))
        self.assertTrue(protocol.admitted("fd7a:115c:a1e0::1"))
        self.assertFalse(protocol.admitted("192.168.1.10"))
        self.assertFalse(protocol.admitted("100.128.0.1"))
        self.assertFalse(protocol.admitted("127.0.0.1"))
        self.assertTrue(protocol.admitted("127.0.0.1", loopback=True))

    def test_a_voice_message_arrives_whole(self):
        self.befriend()
        with open(os.path.join(VECTORS, "voice.opus"), "rb") as f:
            data = f.read()
        mid = self.alice.send_voice("Bob", data, 1500)
        self.alice.wait_for(lambda: self.alice.delivered(mid))
        self.assertEqual(self.bob.message(mid).voice, data)

    def test_a_call_is_answered_carries_audio_and_ends(self):
        self.befriend()
        call = self.alice.call("Bob")
        self.alice.wait_for(lambda: self.alice.calls[call]["state"] == "connected")
        voice = load("voice.json")
        for seq, frame in enumerate(voice["frames"][:5]):
            self.alice.send_call_audio(call, seq, bytes.fromhex(frame["hex"]))
        self.bob.wait_for(lambda: len(self.bob.calls[call]["frames"]) == 5)
        self.alice.end_call(call)
        self.bob.wait_for(lambda: self.bob.calls[call]["state"] == "ended:hangup")

    def test_a_second_call_gets_busy(self):
        self.befriend()
        self.bob.call_policy = "ignore"
        first = self.alice.call("Bob")
        self.bob.wait_for(lambda: self.bob.calls.get(first, {}).get("state") == "ringing")
        second = self.alice.call("Bob")
        self.alice.wait_for(lambda: self.alice.calls[second]["state"] == "declined:busy")

    def test_a_silent_call_is_ended_with_call_end_timeout_which_the_other_side_hears(self):
        self.befriend()
        self.bob.call_silence_s = 0.5
        self.alice.call_silence_s = 60
        call = self.alice.call("Bob")
        self.alice.wait_for(lambda: self.alice.calls[call]["state"] == "connected")
        self.alice.wait_for(lambda: self.alice.calls[call]["state"] == "ended:timeout", timeout=5)
        self.assertEqual(self.bob.calls[call]["state"], "ended:timeout")

    def test_a_cellular_call_ends_a_tailnet_call(self):
        self.befriend()
        call = self.alice.call("Bob")
        self.alice.wait_for(lambda: self.alice.calls[call]["state"] == "connected")
        self.bob.cellular_call()
        self.alice.wait_for(lambda: self.alice.calls[call]["state"] == "ended:cellular")

    def test_a_call_to_an_unreachable_peer_fails_at_once(self):
        self.befriend()
        self.bob.stop()
        time.sleep(0.2)
        call = self.alice.call("Bob")
        self.assertEqual(self.alice.calls[call]["state"], "ended:unreachable")


class WithoutCryptography(unittest.TestCase):
    """The peer on a machine without `cryptography`: the pure-Python Ed25519."""

    def test_two_peers_shake_hands_on_pure_python(self):
        saved = ed25519.HAVE_CRYPTOGRAPHY
        ed25519.HAVE_CRYPTOGRAPHY = False
        self.addCleanup(setattr, ed25519, "HAVE_CRYPTOGRAPHY", saved)
        alice, bob = ChatPeer(name="alice").start(), ChatPeer(name="bob").start()
        self.addCleanup(alice.stop)
        self.addCleanup(bob.stop)
        conn = alice.connect(bob.address)
        self.assertEqual(conn.peer_key, bob.key)


class ConformanceAgainstAReferencePeer(unittest.TestCase):
    """Every conformance scenario, with another reference chat peer as the target."""

    def target(self):
        return conformance.ReferenceTarget()

    def test_every_scenario(self):
        self.assertGreaterEqual(len(conformance.SCENARIOS), 9)
        for scenario in conformance.SCENARIOS:
            with self.subTest(scenario.__name__):
                target = self.target()
                try:
                    scenario(target)
                finally:
                    target.stop()


class ConformanceByAddressAlone(unittest.TestCase):
    def test_the_wire_only_scenarios_need_only_an_address_and_a_key(self):
        target = conformance.ReferenceTarget()
        self.addCleanup(target.stop)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            failed = conformance.main(["--address", "%s:%d" % target.address, "--key", target.key])
        self.assertEqual(failed, 0, out.getvalue())
        self.assertIn("4 of 4 scenarios passed", out.getvalue())


class ConformanceAgainstAPeerProcess(ConformanceAgainstAReferencePeer):
    """The same, driving peer.py as a separate process over its control protocol,
    as a later ticket drives the companion app's headless core."""

    def target(self):
        return conformance.ProcessTarget([sys.executable, PEER_PY, "--listen", "127.0.0.1:0",
                                          "--loopback", "--name", "process"])


if __name__ == "__main__":
    unittest.main()
