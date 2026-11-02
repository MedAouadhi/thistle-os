# Tailnet chat protocol, version 1

Tailnet chat carries messages, voice messages and calls between chat peers on
one tailnet: the T-Deck's app and the companion app on Linux and Android.
`.scratch/tailnet-chat/spec.md` has the product and ADR-0015 the reasons;
`CONTEXT.md` defines **chat peer**, **identity key**, **contact** and
**contact request**. This document is the protocol's authority. The test
vectors beside it (`vectors/`) and the reference chat peer
(`tools/tailnet_chat/`) follow it; where they differ from it, they are wrong.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in
RFC 2119. A rule with none of them describes what happens; it does not
oblige.

## 1. Transport

- A chat peer MUST listen on TCP port **4810** of its tailnet address, and
  SHOULD listen on no other address. A setting MAY change the port
  (`thistle_os.tailnet_chat.port` on the device).
- The connection is a WebSocket (RFC 6455), plain `ws://`, on the path
  **`/tailnet-chat/1`**. There is no TLS: WireGuard encrypts and admits.
  A listener MUST answer any other path with an HTTP error and close.
- **Admission.** A listener MUST close, before the WebSocket upgrade, any
  connection whose source address is outside the tailnet's ranges,
  `100.64.0.0/10` and `fd7a:115c:a1e0::/48` (an IPv4-mapped IPv6 address
  counts as its IPv4 address), and SHOULD also close one that did not arrive
  on the tailnet interface. A simulator or a test peer MAY admit loopback
  as well; nothing else may.
- **Frames.** Every text frame is one JSON object (RFC 8259, UTF-8) with a
  string member `type`: the *JSON frames* of section 4. Every binary frame
  is audio or file data with the header of section 6. A JSON frame MUST
  NOT exceed 16384 bytes. A peer MUST accept fragmented WebSocket messages
  and MUST answer a WebSocket ping with a pong.
- **One connection per pair.** A peer dials when it has something to send
  or a reason to look (section 9), and SHOULD keep at most one open
  connection per other identity key. If two authenticated connections
  between the same two keys exist at once (both dialled together), each side
  MUST keep the one **dialled by the lower identity key** (the 32 key bytes
  compared as unsigned big-endian numbers) and close the other with
  WebSocket close code 4001.

## 2. Encodings

| Value | Encoding |
|---|---|
| identity key | 32-byte Ed25519 public key (RFC 8032), 64 lowercase hex digits |
| nonce | 32 random bytes, 64 lowercase hex digits |
| signature | 64-byte Ed25519 signature, 128 lowercase hex digits |
| message id, call id | 128 bits chosen by the sender at random, 32 lowercase hex digits |
| time (`ts`) | milliseconds since 1970-01-01 UTC, a JSON integer, 0 to 2^53 |
| stream id | an unsigned 32-bit integer, 1 to 4294967295 (0 is not a stream) |

A receiver MUST treat hex in upper case, or of the wrong length, as
malformed (`bad_request`). A receiver MUST accept the members of a JSON
object in any order and MUST ignore members it does not know, so that a
later version can add them. A sender MUST NOT send members this document
does not define, except as a later version defines them.

**Fingerprint.** A key's fingerprint is the first 10 bytes of its SHA-256,
as 20 lowercase hex digits. People compare it in five groups of four
(`630d cd29 66c4 3366 9112`); a file name uses it whole
(`630dcd2966c433669112`).

## 3. The handshake

Both sides send `hello` as soon as the WebSocket is open; neither waits
for the other's. Then the **dialler** sends its `auth`; the **listener**
checks it and only then sends its own.

```
dialler                                 listener
   |---- hello (Kd, Nd) --------------->|
   |<--------------- hello (Kl, Nl) ----|
   |---- auth  sign(Kd, T(dialler)) --->|   listener verifies
   |<-------- auth  sign(Kl, T(listener))|   dialler verifies
   |        ... JSON and binary frames ...
```

```json
{"type":"hello","version":1,"key":"<64 hex>","name":"T-Deck Max","caps":["text","receipts",...],"nonce":"<64 hex>"}
{"type":"auth","sig":"<128 hex>"}
```

- `version` MUST be 1. A peer that receives another sends `error version`
  and closes with 4000.
- `key` is the sender's identity key; `nonce` 32 bytes fresh from a
  cryptographic random source for this connection (MUST NOT be reused).
- `name` is the name the sender shows, at most 64 bytes of UTF-8: the
  device's tailnet hostname, or the companion app's.
- `caps` lists the capabilities of section 5.

**The transcript.** Each side signs, with its identity key, this 149-byte
string:

| Offset | Length | Content |
|---:|---:|---|
| 0 | 20 | the label `tailnet-chat/1 auth` followed by one zero byte: `74 61 69 6c 6e 65 74 2d 63 68 61 74 2f 31 20 61 75 74 68 00` |
| 20 | 1 | the signer's role: `01` the dialler, `02` the listener |
| 21 | 32 | the dialler's nonce |
| 53 | 32 | the listener's nonce |
| 85 | 32 | the dialler's identity key |
| 117 | 32 | the listener's identity key |

The role byte makes the two signatures of one handshake differ, so neither
can be reflected back as the other. `vectors/handshake.json` has a whole
handshake with its keys, nonces, transcripts and signatures, and auths that
must be refused.

**Rules.**

- A peer MUST send nothing but `hello`, then `auth`, before the handshake
  completes, and MUST treat anything else received before it as a failure.
- A listener MUST verify the dialler's `auth` against the key in the
  dialler's `hello` before sending its own `auth`. If it does not verify,
  the listener sends `error bad_signature` and closes with **4000**,
  without sending its `auth`.
- The dialler MUST verify the listener's `auth` likewise, and close with
  4000 if it does not verify. Receiving the listener's `auth` tells the
  dialler that its own was accepted.
- A missing or late signature is a failure too: a peer MUST close a
  connection whose handshake has not completed within **10 s** of the
  WebSocket opening, with 4000.
- A peer MUST refuse (`error bad_request`, 4000) a `hello` that announces
  its own identity key.
- Only after the handshake does either side act on the other's key
  (section 7).

## 4. JSON frames

After the handshake, a peer MUST check each JSON frame and answer one it
cannot take with an `error` frame (section 8), keeping the connection open:

- a `type` it does not know: `error unsupported`, `what` naming the type;
- a known type with a member missing, of the wrong JSON type or out of its
  range: `error bad_request`, `what` naming the type;
- text over its limit: `error too_large` (section 4.1).

`vectors/messages.json` holds one of each frame and invalid frames with the
error each draws. `hello` and `auth` after the handshake are `bad_request`.

### 4.1 `msg`: a text message

```json
{"type":"msg","id":"<32 hex>","ts":1793541600000,"text":"Are you home?","reply_to":"<32 hex>"}
```

- `text` is 1 to **4096 bytes** of UTF-8. Longer draws `error too_large`
  with `what` `msg` and the message's `id`, and is neither stored nor
  acked. Empty is `bad_request`.
- `reply_to`, optional, is the id of the message this one answers (`reply`).
- `ts` is when it was written; a receiver orders a conversation by `ts`,
  not by arrival.
- The receiver MUST store the message before it answers `ack` with its id.
  A receiver that has already stored that id MUST ack it again and MUST NOT
  show it twice.

### 4.2 `ack`: delivered

```json
{"type":"ack","id":"<32 hex>"}
```

The delivered receipt, for a `msg`, `react`, `voice` or `contact`. It is
what clears the sender's outbox (section 9). A sender MUST keep a frame in
its outbox until it is acked.

### 4.3 `read`: read receipts

```json
{"type":"read","ids":["<32 hex>", ...]}
```

1 to 100 ids of messages the reader has read. A peer MUST NOT send `read`
while its user has turned read receipts off, and MUST NOT queue it: it is
sent on an open connection or not at all. It is not acked.

### 4.4 `typing`

```json
{"type":"typing","on":true}
```

The sender's user is typing (`true`) or stopped (`false`). The receiver
treats it as off **6 s** after the last `true`; a sender SHOULD repeat
`true` every 4 to 5 s while typing goes on. Never queued, never acked.

### 4.5 `react`: a reaction

```json
{"type":"react","id":"<32 hex>","ts":1793541609000,"to":"<32 hex>","emoji":"👍"}
```

`id` is the reaction's own id; `to` the message reacted to; `emoji` at most
32 bytes, empty to remove the sender's reaction. One reaction per sender per
message: a later one replaces it. A reaction goes through the outbox and
is acked and deduplicated by its `id` like a message.

### 4.6 `contact`: a contact request answered

```json
{"type":"contact","id":"<32 hex>","state":"accepted"}
{"type":"contact","id":"<32 hex>","state":"declined"}
```

Sent by the user who accepted or declined a contact request (section 7),
through the outbox, and acked.

### 4.7 `voice`: a voice message

```json
{"type":"voice","id":"<32 hex>","ts":1793541600000,"ms":1500,"bytes":6013,"codec":"ogg-opus","stream":1,"reply_to":"<32 hex>"}
```

- The file follows on the same connection as binary frames of kind `01`
  (section 6) on `stream`, sequence numbers from 0, each payload at most
  4096 bytes, in order, adding up to exactly `bytes`.
- `codec` MUST be `ogg-opus`: an Ogg/Opus file (RFC 7845), mono, recorded
  at 16 kHz, 20 ms frames. Another codec draws `error unsupported` with
  `what` `codec:<name>` and the `id`.
- `ms` is 1 to 120000 (2 minutes); `bytes` 1 to 524288.
- The receiver MUST ack only once it holds all `bytes`. A chunk out of
  sequence, on an unknown stream, or past `bytes` draws `error bad_request`
  and drops the transfer. A transfer broken by a closed connection is
  dropped; the sender sends the `voice` frame and the whole file again on
  its next connection (it is still in the outbox).
- `vectors/voice.json` has a voice message, its chunks and its Opus frames.

### 4.8 Calls: `call_offer`, `call_answer`, `call_end`

```json
{"type":"call_offer","call":"<32 hex>","transports":["ws-opus"],"stream":2}
{"type":"call_answer","call":"<32 hex>","transport":"ws-opus"}
{"type":"call_answer","call":"<32 hex>","decline":"busy"}
{"type":"call_end","call":"<32 hex>","reason":"hangup"}
```

- `call_offer` lists the media transports the caller can use, best first.
  v1 defines one, **`ws-opus`**: Opus, 16 kHz mono, 20 ms frames, as
  binary frames of kind `02` on `stream`, one Opus packet each, sequence
  numbers counting frames from 0. Both directions use the same stream id.
- `call_answer` carries exactly one of `transport`, the one picked from
  the offer, or `decline` with a reason: `declined` (the user said no, or
  the caller is not a contact), `busy` (in a call already), `unsupported`
  (no transport in common). Neither or both is `bad_request`.
- `call_end` ends a call, offered or connected, with a reason: `hangup`,
  `cellular` (a cellular call took over the device), `timeout`, `error`.
- An offer nobody answers SHOULD be ended by the caller after **45 s**,
  with `call_end` `timeout`; the callee SHOULD stop ringing at the same
  time and end it the same way.
- A receiver SHOULD play from a jitter buffer of 60 to 200 ms and conceal a
  lost or late frame with Opus' own loss concealment. Either side MUST end
  a connected call after **10 s** without a frame from the other.
- A peer in a call MUST answer another offer with `decline` `busy`. A peer
  MUST decline an offer from a key that is not a contact with `declined`.
- A call to a peer that cannot be connected to fails at once, as
  `unreachable`; nothing is sent, and it is not queued.
- Calls are sent on an open connection, never through the outbox.

### 4.9 `error`

```json
{"type":"error","code":"unsupported","what":"sticker"}
{"type":"error","code":"too_large","what":"msg","id":"<32 hex>"}
```

`code` (section 8); optional `what`, the type, codec or field concerned (at
most 256 bytes); optional `id`, the message concerned; optional `detail`,
for a person reading a log (at most 512 bytes). A peer MUST NOT answer an
`error` with an `error`.

### 4.10 `peers_ask` and `peers`: the tailnet's peer list

```json
{"type":"peers_ask"}
{"type":"peers","peers":[{"name":"thistle-46cf20","addrs":["100.110.81.38","fd7a:115c:a1e0::2e38:5128"]},{"name":"laptop","addrs":["100.101.102.103"]}]}
```

Some platforms list the tailnet's peers (`tailscale status` on Linux, the
device's own client) and some do not (Android, where the Tailscale app
shares none with other apps). A chat peer on one that does not can look
for chat peers among the lists its contacts share. Both frames need the
`peers` capability on both sides (section 5); either from a peer whose
hello did not announce it draws `error unsupported`.

- `peers_ask` asks for the list. It is answered with one `peers` frame.
- `peers` lists the tailnet peers the answering side's platform shows
  online, less the asker: at most **64**, each with `name`, its tailnet
  hostname (1 to 64 bytes), and `addrs`, 1 or 2 of its addresses inside the
  tailnet's ranges (section 1). A peer whose platform has no list answers
  an empty one.
- A peer MUST answer only a **contact** (section 7) with the list, and a
  stranger with an empty `peers`.
- An unasked `peers` is ignored; a malformed one draws `error
  bad_request`.
- **The asker MUST NOT take a key, a name or any trust decision from the
  list.** It is data from another machine. The asker only learns addresses
  to probe with a hello on its discovery port (section 9), as it would
  probe its own platform's peers, and MUST drop any address outside the
  tailnet's ranges. A chat peer found that way is added by the address it
  answered on, and its key pinned at the first handshake, as with any
  address the user adds.

Neither frame is queued or acked: both are sent on an open connection.

## 5. Capabilities

v1 announces: `text`, `receipts`, `typing`, `reply`, `react`,
`voice-message`, `call:ws-opus`, `peers`.

- A peer MUST use a feature only if both hellos announce it: no `read`
  unless both have `receipts`, no `typing` frame unless both have
  `typing`, no `reply_to` unless both have `reply`, and so on. `text` is
  what `msg` needs. A queued `react` or `voice` the other side did not
  announce stays queued for a connection whose hello does; a queued `msg`
  goes without its `reply_to`; a call to a peer without `call:ws-opus`
  ends at once as `unsupported`, with nothing sent; a peer without
  `peers` is not asked for its peer list (section 4.10).
- A peer MUST ignore a capability it does not know.
- **Room for later, with no new version.** Video: a `webrtc` capability and
  a `webrtc` transport in `call_offer`, its SDP and ICE exchanged as JSON
  frames on the same connection, between peers that both announce it.
  Files: a `file` capability, a `file` message and its chunks as binary
  frames of kind `03`, like a voice message but resumable. A v1 peer
  answers either message with `error unsupported`, and never sees one from
  a peer that read its hello.

## 6. Binary frames

| Offset | Length | Content |
|---:|---:|---|
| 0 | 1 | kind: `01` voice message data, `02` call audio, `03` reserved for files |
| 1 | 4 | stream id, unsigned, big-endian |
| 5 | 4 | sequence number, unsigned, big-endian |
| 9 | 0 to 4096 | payload |

A frame shorter than 9 bytes or with more than 4096 bytes of payload is
malformed: `error bad_request` with `what` `binary`. A kind the receiver
does not know draws `error unsupported`. `vectors/frames.json` has headers
and malformed frames.

## 7. Contacts and trust

After the handshake each side classifies the other's proven key:

1. **A contact**: its key is pinned. Its frames are taken.
2. **A pinned contact's mismatch**: the key differs from the one pinned for
   a contact the connection matches by **name or address**: the address
   the peer dialled, the source address of an incoming connection, or the
   name (the tailnet hostname where the platform knows it, otherwise the
   hello's `name`). The peer MUST send `error key_changed`, close with
   **4002**, and flag the contact for its user. Its frames are not taken.
   Only the user accepting the new key in the old one's place ends this.
3. **A stranger**: anything else. Its `msg`, `react` and `voice` frames are
   acked, so its outbox clears, but **held** as a contact request, shown
   with its `name` and its key's fingerprint; they MUST NOT be shown as a
   conversation. `read` and `typing` from a stranger are ignored, and its
   call offers declined.

A key is pinned when the user adds a chat peer by its address or name and
the first handshake with it completes (the tailnet trusted once), or when
the user accepts a contact request.

- **Accepting** a request pins the stranger's key, shows the held
  messages in `ts` order, and sends `contact accepted` through the outbox.
- **Declining** drops the held messages and sends `contact declined`. A
  peer that receives `contact declined` MUST drop its outbox for that key
  and MUST NOT send it messages again until its user starts over.
- Removing a contact unpins it: its next message is a contact request again.

## 8. Errors and close codes

| `error` code | Meaning | The connection |
|---|---|---|
| `unsupported` | a type, codec, transport or binary kind the receiver does not know; `what` names it | stays open |
| `bad_request` | a malformed frame, or one out of place | stays open (closed with 4000 in the handshake) |
| `too_large` | text over 4096 bytes or a frame over 16384 | stays open |
| `version` | `hello` with a version other than 1 | closed, 4000 |
| `bad_signature` | an `auth` that does not verify | closed, 4000 |
| `key_changed` | a contact's name or address with another key | closed, 4002 |

| WebSocket close code | Meaning |
|---|---|
| 1000 | idle, or done |
| 1001 | the peer is going away (stopping, sleeping) |
| 4000 | the handshake failed |
| 4001 | a duplicate connection between the same keys (section 1) |
| 4002 | key changed (section 7) |

A connection closed for any reason loses nothing: what was not acked is
still in its sender's outbox.

## 9. The outbox and liveness

- Every `msg`, `react`, `voice` and `contact` waits in its sender's outbox
  until acked. A sender SHOULD retry when it reconnects, when the peer is
  seen again in the tailnet's peer list, and on a backoff from 30 s
  doubling to 30 minutes; nothing expires. `typing`, `read` and calls are
  never queued.
- A sender MAY send the same frame again on a new connection; the receiver
  dedupes by id.
- A peer SHOULD send a WebSocket ping every **20 s** while a connection is
  open, and SHOULD close a connection with nothing to send, no transfer and
  no call after **2 minutes** idle (1000). Idle means no text or binary
  frame sent or received: pings and pongs do not count, or the pings
  would keep every connection open.
- Every chat peer answers a hello whenever another probes it: a probe is a
  dial, a `hello`, and a close.

## 10. What this version does not protect against

WireGuard encrypts and admits; the identity keys say who is at the other
end. Nothing in the WebSocket is encrypted or bound to the handshake again
(ADR-0015). So a node on the tailnet that a contact can be made to dial
under another contact's name could relay a handshake between the two;
nothing in v1 prevents it, and the trust placed in the tailnet covers it.
