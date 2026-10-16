# 04: The simulator dictates

Status: in-review

**Blocked by:** 02

**What to build:** The simulator's microphone returns real audio:
`THISTLE_SIM_MIC=FILE.wav` plays a 16 kHz mono WAV into it in real time,
and otherwise a windowed run opens the host's microphone through SDL.
`simulator/tests/test_dictation.py` runs against a fake Cohere server.

- [x] `sim_audio.c` reads a WAV, or SDL capture, paced as the codec is
- [x] test: a dictation into the Terminal's input arrives, and the request had the Bearer key, the model, the language and a 16 kHz mono WAV
- [x] test: no field focused, a tap, and a 401 each send nothing or change nothing
- [x] The simulator's `config/dictation.json` is git-ignored

## Comments

The host microphone is opened through SDL only in a windowed run, and closed
700 ms after the last read, as the driver closes the codec, so the desktop
does not show it in use. The first 400 ms after it opens are dropped, as on
the device. SDL capture has not been tried here: every test is headless and
uses `THISTLE_SIM_MIC`.
