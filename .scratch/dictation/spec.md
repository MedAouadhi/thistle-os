# Dictation

Status: built and tested in the simulator; waiting on the hardware pass (a Cohere key in `.env`)

## Problem Statement

Typing on the T-Deck Max's keyboard is slow, and the user already dictates
to Hermes on the other device (`~/personal/hermes_esp32`): hold
push-to-talk, speak, let go, and the words arrive as text. ThistleOS should
do the same in every text field, in every app, with or without Hermes or
the tailnet.

## Solution

Hold the microphone key (bottom row, left of Space) while a text field has
the keyboard's focus. A pill at the top of the screen says **Listening**
once the microphone is live; let go and it says **Transcribing**. The
transcript is inserted at that field's cursor and the pill goes.

- **No field focused, nothing records.** A toast says to pick a text field.
- **Password fields are refused**, so a spoken password never leaves the
  device.
- **A tap is not a dictation.** Under 300 ms, nothing is sent, and a toast
  says to hold the key.
- **60 seconds at most.** At 50 s the pill counts down; at 60 s it stops and
  transcribes as if the key were let go. Hermes has the same limits
  (hermes_esp32 ADR 0005).
- **Sym or Alt, then the mic key, still types `0`**, the key's printed
  second legend.
- **Failures say so** and leave the field as it was: no network, no API key,
  the service refused, or nothing heard.
- **Not during a call.** The call has the audio.
- **A podcast pauses while you dictate** and plays on once the recording
  ends, while the words are being transcribed (ticket 03).
- LVGL window managers only, as for the quick panel; thistle-tk has no text
  fields to dictate into.

## Transcription

The same provider, model and language as the Hermes agent on `homelab`
(Hermes's `stt.provider: cohere api`, `~/.hermes/scripts/cohere_api_stt.py`):

- `POST https://api.cohere.com/v2/audio/transcriptions`, multipart form
  data: `model`, `language`, `file`. `Authorization: Bearer <token>`.
  The answer is `{"text": "..."}`; an error is `{"message": "..."}`.
- Model `cohere-transcribe-arabic-07-2026`, language `ar` until the user
  picks English on the quick panel's **Dictation** tile (EN lit).
- **Cohere detects no language**: "It does not feature explicit, automatic
  language detection" (docs.cohere.com/docs/transcribe). Told `ar`, it
  answered one English sentence on the T-Deck Max in English and the next
  in Arabic, translated: "هذه أخرى نتيجة، هل تعمل؟". Hermes's evidence of
  English coming back as English with `ar` holds only some of the time. So
  the user says which language they speak.
- The audio is a 16 kHz mono 16-bit PCM WAV: what the Hermes bridge hands
  the provider, and what the provider resamples anything else to.

## Configuration

- **The key is never in the source tree.** CMake reads `COHERE_API_TOKEN`,
  the name Hermes's `.env` uses, from the repository's `.env` (git-ignored)
  into a header in the build directory, as the Podcasts app does.
- **`config/dictation.json` on the SD card** overrides any of `api_key`,
  `model` and `url`. The simulator's copy is git-ignored.
- **The language is the user's**, in system.json as
  `thistle_os.dictation.language` (`dictation_prefs.rs`), set by the quick
  panel's tile: `en` or `ar`. Any other code the model knows can be written
  there by hand. The pill says which: "Listening (AR)".

## Decisions

- **Hold to talk, not a toggle**, as on Hermes. Letting go is the "send".
- **The field focused at the press gets the text**, even if focus moved
  meanwhile. If that field is gone (its app was left), the text is dropped
  and a toast says so.
- **A space is put between the transcript and a word before the cursor**;
  in a one-line field the transcript's line breaks become spaces.
- **The recording is held in PSRAM, whole, then uploaded.** About 1.9 MB
  for 60 s, allocated at the press and freed after: there is no streaming
  upload to stream to, and 8 MB of PSRAM. The WAV header and the multipart
  framing are written around the samples in the same allocation, so
  nothing is copied.
- **One FreeRTOS task per dictation**, 8 KB of internal RAM for the TLS
  handshake, as the Podcasts threads have; it ends when the transcript is
  in. Not a `std::thread` (CLAUDE.md's Tailscale note).
- **The device is held awake** from the press until the transcript is in.
- **The codec's first 400 ms are dropped by the driver** (ticket 12 of
  `tdeck-max`), so "Listening" appears once audio is actually arriving:
  speaking before it shows loses words.
- **TLS needs no clock here**: mbedTLS is built without
  `MBEDTLS_HAVE_TIME_DATE`, so certificate dates are not checked.

## Tickets

- 01: the microphone key reaches the window manager (`HAL_KEY_MIC`).
- 02: dictate into the focused text field.
- 03: playback pauses for dictation.
- 04: the simulator dictates: a WAV file or the host's microphone, and a
  fake Cohere in the tests.
- 05: Arabic text on screen.

## Out of scope

- A Settings page for the key and model (the card's
  `config/dictation.json` does it for now).
- Showing Arabic: the only font is Montserrat, Latin only, and LVGL is built
  without Arabic shaping or right-to-left text, so an Arabic transcript is
  inserted correctly but drawn as boxes. Ticket 05.
- Dictation into thistle-tk.
- Offline transcription.
