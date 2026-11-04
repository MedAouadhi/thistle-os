# 08: Device: voice messages

**What to build:** in a conversation the user holds to record a voice message of up to 2 minutes, in Opus at 16 kHz mono, and sends it; it goes through the outbox in chunks and is acked once whole. A received one is kept on the card beside the conversation's history, shown with its length, and played when the user chooses. A player such as Podcasts pauses while one records or plays and resumes after. The device is held awake during a transfer.

**Blocked by:** 01, 04

**Status:** done in the simulator; not yet heard on hardware

- [x] Recording takes the audio as dictation does and stops at 2 minutes; under 300 ms is a tap, not a message.
- [x] The file is Ogg/Opus as the protocol's vector is; it is kept as a `.opus` file beside the history and logged as a message that points to it.
- [x] A transfer broken off is sent again from the beginning on the next connection; a half-received file is never shown.
- [x] The whole recording is in PSRAM; the encoder's and decoder's stacks are as ticket 01 measured.
- [x] The conformance suite's voice scenarios (whole and broken) pass against the simulator, which records from `THISTLE_SIM_MIC` and writes what it plays to a file the test reads.

## Comments

Done on branch `tailnet-chat-08-device-voice`.

- **The wire** (`tailnet_chat_wire.rs`): `CAPS` gains `voice-message`. `voice()` builds the frame, `voice_chunk(stream, data, seq)` the binary chunks (4 KB each), and `VoiceIn` receives a transfer: `take()` says whole, or the `bad_request` that drops it (another stream, out of sequence, past `bytes`). Tested against `vectors/voice.json`: the frame and both chunks are the vector's byte for byte.
- **Sending** (`tailnet_chat.rs`): `send_voice(key, data, ms, reply_to)` (C `tailnet_chat_send_voice`, control port `send_voice {to, file, ms}`) keeps the file at `messages/tailnet/voice/<id>.opus` (written as `.tmp` and renamed), puts the `voice` frame in the outbox like a text, and logs a line with `"voice":"voice/<id>.opus","ms","bytes"` and empty text. The outbox holds only the frame; on each connection `flush` reads the file again from the card into a `VoiceOut` (PSRAM: over 4 KB), and `pump_voice` puts the frame, then the chunks, into tx while it holds under 16 KB, one voice message after another, so the 64 KB tx cap never trips and nothing else waits behind a whole file. A file gone from the card drops its entry from the outbox with a warning. Broken off, the entry is still in the outbox and goes again whole on the next connection.
- **Receiving**: a `voice` frame starts a `VoiceIn` (its room taken at once, PSRAM); each chunk is checked; whole, the file is kept beside the history and only then the message is shown (or held, from a stranger) and acked. A duplicate is acked without keeping the file again. A received voice file is kept even with `save_history` off: it is the message, not its history.
- **Awake**: a connection holds the device awake (`HoldGuard`, "a voice message") while it receives a transfer, has one to send, or waits for a sent one's ack.
- **Shown**: `Message.voice` (`ChatVoice {ms, bytes}`), read back from the file by `fold`. The C message has `voice_ms` and `voice_bytes` (appended); its text, the request preview and a reply's quote say "Voice message, 0:02". `tailnet_chat_voice_path(id)` gives the file only once it is whole.
- **The app** (`apps/tailchat/`): a button right of Send (`LV_SYMBOL_AUDIO`) records while held and sends when let go; the note says "Recording 0:05 of 2:00; let go to send". A voice message's bubble is "Lena: Voice message 0:02" (", playing" while it plays); choosing it offers Play (Stop while it plays). `tailchat_ogg.c` writes and reads Ogg/Opus (one stream, 50 packets a page, CRC checked, pre-skip from the encoder's lookahead), host-tested against `voice.opus`. `tailchat_voice.c` records and plays on a FreeRTOS task each, made for it and gone after, 20 KB internal stack (ticket 01's measure), everything else in PSRAM: 20 ms frames encoded as they come, so only the packets are kept (under 500 KB), then the file (512 KB at most); playing decodes 200 ms at a time. Both call `rs_audio_capture_begin`/`_end`, so Podcasts pauses and plays on after, refuse during a cellular call, and take Opus with the new `thistle_opus_take()`/`_give()` (one user at a time: the pseudostack is shared). Leaving the app stops a playback and drops a recording under way.
- **With calls (09) and sweeps (07)**, merged: both caps are announced; a call takes Opus too, waiting up to a second for a voice message to give it back, and a voice message recording or playing stops when a call comes (a recording cut so is dropped); a connection a sweep opened is not closed in the middle of a voice transfer.
- **The simulator**: 09's `THISTLE_SIM_SPEAKER=FILE.wav` (everything played, as a WAV) is the speaker the test reads (mine, written the same day, gave way to it), and `--tap X,Y:MS` holds a touch. `test_tailchat.py`: the conformance case runs every scenario but the calls (ticket 09 adds them), 18 of 18 including `voice_message_whole_and_broken`, `voice_message_from_target` and `voice_message_broken_and_restarted`, and checks the received file on the card is the vector's; the new `voice` case holds the button 120 ms (a tap: "Hold to record") and 3 s with `THISTLE_SIM_MIC` a tone (2600 ms recorded: the codec's 400 ms settle; a file `tools/tailnet_chat/ogg.py` reads, 130 frames, in the outbox and the history), then plays the vector from the card on Play: 1513 ms at 16 kHz mono, RMS 2886, to the speaker file.
- **Simulators side by side**: the message log in the simulator now writes under `THISTLE_SIM_SDCARD` when it is set, not the machine-wide `/tmp/thistle_sdcard` symlink, which another session's simulator could repoint mid-run (it did, three times, while tickets 07 and 09 ran theirs). The rest of the kernel still uses the symlink.
- **The WASM build** now builds libopus too, as the desktop simulator does (the Chat app links it). Not built here: no emscripten on this machine; Pages' run will tell.
- **Measured**: the firmware, with 07 and 09 merged, is 0x40ff20 bytes (4.06 MiB) against the 4.5 MiB kernel slot. Not measured: the recording and playing tasks' stack high-water on the device (logged as `recording done; N bytes of stack to spare`), and how loud the T-Deck Max's playback is.

### For hardware (ticket 12)

- Record 2 minutes and check it stops at 2:00 and the file is about 240 KB or less; send it over LTE (DERP).
- Play a received one with Podcasts playing: it pauses, then plays on.
- The stack lines from `tailchat_voice`, and the free internal RAM while recording (one 20 KB stack).

### Left

- A declined contact request's voice files stay in `voice/` (its history file is deleted); so do a removed contact's. Nothing cleans `voice/` yet.
- From the keys, Enter held on the focused voice button records while held (untested); there is no key of its own, as the mic key dictates.
