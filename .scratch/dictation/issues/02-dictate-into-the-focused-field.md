# 02: Dictate into the focused text field

Status: in-review

**Blocked by:** 01

**What to build:** Holding `HAL_KEY_MIC` with a text field focused records
from the microphone; letting go sends the recording to Cohere and inserts
the transcript at that field's cursor. See the spec for the rules.

- [x] `dictation_core.c`: the rules that need no hardware, host-tested: a tap, the 50 s warning and the 60 s stop; the WAV header; the multipart framing; reading `text` or `message` from the answer, `\u` escapes and surrogate pairs included; the space before the transcript; line breaks in a one-line field
- [x] `dictation.c`: the session on the LVGL side (the field, the pill, the toasts, inserting the text) and a task for the recording and the upload
- [x] The field is checked at the press: focused in the keypad's group, a textarea, not a password field
- [x] The field going away while the words are on their way drops them, with a toast, and crashes nothing
- [x] Refused during a call, with no network, with no key
- [x] The key from `.env`'s `COHERE_API_TOKEN` into a build-directory header; `config/dictation.json` overrides it, the model, the language and the URL
- [x] Held awake from the press to the transcript
- [x] The recording buffer in PSRAM, freed after; the task's stack internal, and gone after
- [ ] Verified on hardware: English and Arabic, into the Notes app and the Messenger

## Comments

The tap, the warning and the stop are timed by the audio captured, not by
the key: the codec drops its first 400 ms, so a key held 600 ms may have
caught 200 ms of speech, which is a tap.

25 host tests in `test_dictation_core.c`. In the simulator, a dictation into
the Terminal arrives as the fake Cohere's transcript; the request carried the
Bearer key, the model, `ar`, and a 16 kHz mono WAV whose samples are the
microphone's from its first, byte for byte. With no field, nothing records;
a tap sends nothing; a 401 leaves the field alone and says the key was
refused.

Each dictation logs `N.N s recorded; peak X dBFS, rms Y dBFS`, which is what
the microphone gain (24 dB, the driver's default) should be tuned by.

Still to do: the hardware pass, which needs `COHERE_API_TOKEN` in `.env`.

### On the T-Deck Max

Two English sentences, dictated into Notes:

| | Recorded | Peak | Loud speech (p90 of 100 ms) | Noise (p10) | Answered | Back |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 6.9 s, 216 KB | -19.8 dBFS | -37.4 dBFS | -45.1 dBFS | 200 in 2561 ms | "This is a test, just another test to see if it works or not" |
| 2 | 3.8 s, 119 KB | -16.9 dBFS | -37.9 dBFS | -45.6 dBFS | 200 in 1744 ms | "هذه أخرى نتيجة، هل تعمل؟" |

A temporary build dumped both recordings over the console: every byte
arrived, with no run of zeros, no clipping and no DC offset. The second was
translated, not misheard: the language sent was `ar`, and Cohere detects no
language of its own. Hence the quick panel's Dictation tile and
`dictation_prefs.rs`. The Arabic was inserted and drawn as boxes (ticket 05).

"Transcription took a very long time" was the dump, 26 s and 16 s on the
console; the request itself is the 1.7 to 2.6 s above. The task had 4616
bytes of its 8 KB stack to spare.

The recording is quiet: loud speech only 8 dB over the noise, peaks near
-17 dBFS at the driver's 24 dB of gain. The first sentence came back word for
word, so the gain is left alone; if accuracy suffers, more gain is the first
thing to try.
