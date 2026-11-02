# 09: Device: live calls

**What to build:** 1:1 voice calls between the device and a contact. The user calls from a conversation or a contact; an incoming call rings on the system call screen, extended to show a tailnet contact, over whatever app is in front; answer, decline, hang up, mute and volume work. Audio is `ws-opus` with a jitter buffer and Opus' loss concealment, and the microphone is turned down while the far end is loud. One call at a time: a second gets busy. A cellular call that rings ends a tailnet call; an SMS that arrives during a tailnet call over LTE is fetched after it. A call to an unreachable contact fails at once, and a missed one goes into the call log.

**Blocked by:** 01, 04

**Status:** done in the simulator; not yet heard on hardware

- [x] The call rules are a host-tested policy: one at a time, busy, cellular wins, SMS deferred, 10 s without a frame ends the call.
- [x] A call runs on a task made for it and gone after, with an internal stack sized from ticket 01; it holds the device awake, takes the audio, sets the codec to 16 kHz mono, and restores the player's format after.
- [x] The call screen shows a tailnet contact's name and the call's state, from the LVGL window managers.
- [x] The log says each call's start, transport, end and reason, and frames lost.
- [x] The conformance suite's call scenarios (answered, declined, busy, ended by a cellular call, the far end gone silent) pass against the simulator, using the fake modem's `--call` for the cellular case; the test checks the call screen by screenshot and that the simulator plays what the reference peer sends.

## Comments

Done on branch `tailnet-chat-09-device-calls`.

**Where it is.**

- `components/kernel_rs/src/tailnet_call_policy.rs` (host-tested, 15 tests): `answer_offer` (stranger `declined`, no `ws-opus` `unsupported`, a tailnet or cellular call up `busy`, else ring), `may_place`, `cellular_ends`, `sms_waits`, and `Call` with its phases (reaching, offered, ringing, connected, over as `ended:<reason>` / `declined:<reason>`), `due()` (10 s silent `timeout`, 45 s unanswered `timeout`, 7 s to reach a contact `unreachable`) and `log_kind()` (missed, rejected, incoming, outgoing).
- `tailnet_call_media.rs` (host-tested, 10 tests): the jitter buffer (60 ms to start, a frame more each time it runs dry, 200 ms at most, late packets dropped, a sender that restarts followed; the Dart core's `jitter.dart`, ported), and echo suppression (`Ducker`: the microphone -20 dB while what the speaker played is over about -30 dBFS, and 200 ms after).
- `tailnet_call.rs`: the call's task, `kernel_task::spawn` "call", 20 KB internal stack, priority 7. Ringing: the call manager's ringtone (now `pub(crate) ring_tone`) unless silenced or muted, the motor every 2 s. Connected, each 20 ms: 320 samples from `record`, ducked or zeroed when muted, encoded (libopus through FFI, gated to the S3 and the simulator), handed to `tailnet_chat::pump_call`, which writes it on the call's connection and reads what came; the next jitter-buffer frame decoded or concealed, scaled by the call volume (70 % plays as received), to `play`. It holds the device awake ("a tailnet call"), `rs_audio_capture_begin/end` around it, configures 16 kHz mono 16-bit, `stop()` after; it pokes the network coordinator at the end and logs its unused stack. A call placed while the last one's task is still finishing is taken on by that task.
- `tailnet_chat.rs`: `call_offer` / `call_answer` / `call_end` and call audio (kind 02) on the stream of the live call; `calls_due` in each poll (offer sent once the dialled contact's handshake is done, unreachable once nothing reaches it, the policy's timeouts, `error` if the connection goes); `place_call` (dials the contact if not connected), `answer_call`, `hangup`, `cellular_took_over`, `call_view`, `call_in_progress`, `sms_waits`; the call log line on each end; `CAPS` gains `call:ws-opus`. An open connection with a live call is never closed as idle. C API: `tailnet_chat_call(key)`.
- `call_manager.rs` / `call.h`: `call_status_t.tailnet` (appended; the struct is 124 bytes, `link_deps.c` and the layout test updated). While no cellular call is up, `rs_call_get_status` shows the tailnet call and `rs_call_answer/hangup/silence/set_mute/set_volume` act on it (`set_speaker(false)` and DTMF are not supported); `rs_call_in_progress` covers it. Starting a cellular call thread (ringing or dialled) calls `tailnet_chat::cellular_took_over()`. `rs_call_dial("tailnet:<fp>")` calls the contact back. `phonebook::name_for` answers a `tailnet:` number with the contact's name, so the Phone app's Recents and Home's missed-call row show it.
- `net_coordinator.rs`: SMS are not fetched while `tailnet_chat::sms_waits()` (a tailnet call while WiFi is not connected, i.e. over LTE).
- `call_ui.c`: a tailnet call shows the contact's name, "Tailnet call" under it, Mute, Speaker and volume, no Keypad; logs `tailnet call: NAME` when it opens.
- The Chat app: a call button on the conversation's compose row and "Call" on the contact page.
- Simulator: `THISTLE_SIM_SPEAKER=FILE.wav` (sim_audio.c) gets everything `play()` is given; `sim_cellular_ring_now()` rings the fake modem at once; the control port's `call`, `calls`, `answer_call` / `end_call` (through `rs_call_answer` / `rs_call_hangup`, as the screen does), `talk`, `cellular_call` (rings, waits for the tailnet call to end, declines the cellular one and waits for its thread) and `screenshot`.

**Decisions beyond the spec.**

- One call screen and one C API (call.h) for both kinds of call, not a second screen; the cellular call wins the screen when both exist.
- An offer rings for 45 s at most (as the companion's); then `call_end timeout` and a missed call. The same 45 s for an offer of ours.
- A call to a contact with no open connection dials it and has 7 s to finish the handshake; a dial that fails is `unreachable` at once.
- A connection lost under a call ends it as `error`.
- "Over LTE" is "WiFi not connected", as esp_netif routes over WiFi whenever it is up (ADR-0003).
- The call log records a tailnet contact as `tailnet:` + its 20-hex fingerprint id (28 chars, fits `CALL_NUMBER_MAX`).
- The player's format: the call does not save and restore it; a player paused for the call (`rs_call_in_progress`) sets its own format again when it plays on, as Podcasts does after any pause.

**Tests.**

- Kernel host tests: 2207 pass (25 new, plus the hello caps and the call status layout).
- `simulator/tests/test_tailchat.py`: all six cases pass, the new `calls` case running the suite's seven call scenarios (answered, placed by the target, declined, busy, ended by cellular through the fake modem, far end silent, unreachable): 7/7. It also opens a call from "Lena", sends 60 vector frames, screenshots the call screen (name, "Tailnet call", 00:00, Mute, Speaker, volume, End call) and checks the speaker file has over 4000 loud samples after the call connected.
- `test_calls.py` 24/24, `test_phone_app.py` 24/24, the app matrix, `thistle_sim_tests` 158/158, `apps/tailchat/tests/run_tests.sh`, every `tools/test_*.py`.
- `idf.py build`: the kernel slot is 89 % used (4,234,464 of 4,718,592 bytes).
- Note: simulators started by other sessions at the same time share `/tmp/thistle_sdcard` and break each other's card-reading cases (`app`, the Phone app's star); run them when no other simulator is up.

**Hardware, for ticket 12.**

- A call over WiFi and over LTE through DERP; listen for the echo with the speaker loud; the task's stack line (`the call task left N of 20480 bytes of stack unused`) and the free internal RAM with a call up.
- Whether one task's sequential `record` (20 ms) then `play` keeps up on the ES8311 with an encode of 6 ms; the summary line's concealed count says.
- Each received frame is a small heap allocation (about 60 bytes, internal RAM) until it is decoded.
