# 01: A Bridge Session over the tailnet

Status: in-review

**What to build:** The app opens a WebSocket to the bridge once the device
is on the tailnet, says `HELLO`, and keeps the session up while it is open.

- [x] `hermes_ws.c`: RFC 6455 client framing, host-tested: the handshake
      and its accept key, masked frames out, frames in whole or in pieces,
      fragmented messages, ping and close, a message over the limit
- [x] `hermes_proto.c`: `HELLO`, and `HELLO_ACK` or `ERROR` read back,
      host-tested against hermes_esp32's fixtures
- [x] `hermes_bridge.c`: the task; connect with a deadline, reconnect a
      second after a loss and up to 30 s apart, stop within a second
- [x] The Tailscale gate: off, signing in, connecting, failed, no client
- [x] The bridge's address: `.env`'s `HERMES_BRIDGE_URL`, the card's
      `config/hermes.json`, a setup page with the tailnet's peers
- [x] Refused (`AUTHENTICATION_FAILED`): the tag to give the device, and
      tried again every 30 s so it comes up once the tag is given
- [x] `PROTOCOL_INCOMPATIBLE`: which side to update; not tried again

**Blocked by:** nothing

## Comments

Host tests: `test_hermes_ws.c` (RFC 6455's own handshake and masking
examples, frames whole and a byte at a time, fragments with a ping between
them, each way a frame can be wrong) and `test_hermes_proto.c` (the
handshake and its refusals, written as the bridge writes them).

In the simulator, against the fake bridge: the upgrade, HELLO's seven
capabilities, every frame masked, the bridge's pings answered; a refusal
said with the tag to give and tried once in six seconds; an incompatible
plugin said and not tried again; a session closed by the bridge opened again
a second later; nothing tried with Tailscale off; with no address, the page
opens by itself and what is typed there is kept in `config/hermes.json`.

Against hermes_esp32's own `HermesDeviceBridge` (its test kit's fake
tailscaled on loopback, run by hand, not committed): a tagged node is
registered and synced, an untagged one refused with the plugin's words.

A send to a socket the far end closed raised SIGPIPE in the simulator, which
ended it; sends pass `MSG_NOSIGNAL` where there is one. lwIP has no signals.
