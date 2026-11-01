# Tailnet chat: the protocol, its vectors and the reference chat peer

- `protocol.md`: the protocol, version 1. The authority.
- `vectors/`: test vectors, language-neutral JSON plus one Ogg/Opus file,
  for the Rust kernel module's tests and the Dart companion core's
  (`vectors/README.md`).
- `tools/tailnet_chat/`: the reference chat peer, standard-library Python
  (Python 3.10 or newer; `cryptography` is used for Ed25519 when installed,
  a pure-Python RFC 8032 implementation when not):
  - `protocol.py`: everything without I/O: the transcript, the fingerprint,
    the binary header, checking a JSON frame (`check()` returns the frame or
    the `error` to answer), admission;
  - `peer.py`: `ChatPeer`, which listens and dials, and a command-line peer
    driven by the control protocol below;
  - `conformance.py`: the conformance suite;
  - `make_vectors.py`: writes `vectors/*.json`;
  - `ed25519.py`, `ws.py`, `ogg.py`: Ed25519, the WebSocket, reading Ogg.
- `tools/test_tailnet_chat.py`: the reference peer's own tests (CI's host
  tests run them).

## Running

```sh
python3 tools/test_tailnet_chat.py                 # the peer's tests, about 15 s
python3 tools/tailnet_chat/make_vectors.py --check # the vectors are current
python3 tools/tailnet_chat/peer.py --listen 127.0.0.1:4810 --loopback --verbose
```

The conformance suite runs every scenario against a fresh target:

```sh
# another reference peer, in the same process
python3 tools/tailnet_chat/conformance.py --reference
# any program that speaks the control protocol
python3 tools/tailnet_chat/conformance.py \
    --control 'python3 tools/tailnet_chat/peer.py --listen 127.0.0.1:0 --loopback'
# a running chat peer known only by its address and key: the wire-only scenarios
python3 tools/tailnet_chat/conformance.py --address 127.0.0.1:4810 --key <64 hex>
# one scenario
python3 tools/tailnet_chat/conformance.py --reference --only key_changed_refused
```

From Python, `conformance.run(make_target, scenarios)` takes any factory
of `Target`s: a target for the simulator implements `Target`'s methods
(`add_contact`, `contacts`, `requests`, `accept`, `decline`,
`accept_new_key`, `messages`, `send_text`, `waiting`, `stop`, and for the
message scenarios `react`, `mark_read`, `set_typing`, `is_typing`,
`set_read_receipts`, `flush`) with the
simulator's keys, screen and card, and gives its `address` and `key`.

## The control protocol

A chat peer under test, run as a program, reads one JSON object a line on
stdin and answers each with one line on stdout. Its first line, before any
command, is

```json
{"ready": {"key": "<64 hex>", "fingerprint": "xxxx xxxx xxxx xxxx xxxx", "name": "...", "address": ["127.0.0.1", 41234]}}
```

A command is `{"cmd": NAME, ...arguments, "seq": N}`; the answer is
`{"ok": RESULT, "seq": N}` or `{"error": "why", "seq": N}`. Logs go to
stderr, never stdout. `to` names a chat peer: its key in hex, a contact's
name, or an address as `[host, port]`.

| `cmd` | Arguments | Result |
|---|---|---|
| `identity` | | as `ready` |
| `add_contact` | `address` `[host, port]`, `name`?, `key`? | `{"name"}` |
| `contacts` | | `[{"name", "key", "address", "tailnet_name", "flagged_key", "accepted_by_them", "declined_by_them"}]` |
| `requests` | | `[{"key", "name", "fingerprint", "held": [message]}]` |
| `accept` / `decline` | `key` | `true` |
| `accept_new_key` | `contact` (a name or key) | `true` |
| `send_text` | `to`, `text`, `reply_to`? | the message id |
| `react` | `to`, `message`, `emoji` | the reaction's id |
| `send_voice` | `to`, `file` (an Ogg/Opus path), `ms` | the message id |
| `mark_read` | `to`, `ids`? | `true` |
| `typing` | `to`, `on` | `true` |
| `set` | `read_receipts`?, `call_policy`? (`answer`, `decline`, `busy`, `ignore`) | `true` |
| `messages` | `to` | `[message]` |
| `waiting` | `to` | the ids still in the outbox |
| `is_typing` | `key` | boolean |
| `flush` | `to`? | `true` |
| `call` | `to` | the call id |
| `calls` | | `{call id: {"peer", "state", "frames"}}` |
| `end_call` | `call`, `reason`? | `true` |
| `stop` | | `true`, then the program exits |

A message is `{"id", "mine", "ts", "state", "text"?, "reply_to"?,
"voice_bytes"?, "ms"?, "reactions"?}`; `state` is `waiting`, `delivered`
or `read` for one of the peer's own, `received` or `read` for one it
received. A call's `state` is `offered`, `ringing`, `connected`,
`declined:<reason>` or `ended:<reason>`.

The companion app's headless Dart core (`companion/core/`) implements the
same commands, plus `rename` and `remove_contact` (`to` or `contact`, and
`name`), so the suite runs against it as against the reference peer:

```sh
cd companion/core && dart pub get && dart test && dart build cli && cd ../..
python3 tools/tailnet_chat/conformance.py \
    --control 'companion/core/build/cli/linux_x64/bundle/bin/tailnet_chat_core --listen 127.0.0.1:0 --loopback'
```

Its options: `--identity FILE` (the seed in hex, made on first start;
without it a key for this run only), `--db PATH` (SQLite; `:memory:` by
default), `--address HOST`, `--port N` (or `--listen HOST:PORT`), `--name`,
`--loopback`, `--no-read-receipts`, `--call-policy`, `--verbose`.
