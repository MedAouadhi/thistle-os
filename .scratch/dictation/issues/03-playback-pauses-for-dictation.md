# 03: Playback pauses for dictation

Status: in-review

**Blocked by:** 02

**What to build:** A podcast playing when the mic key goes down pauses, and
resumes when the transcript is in. Two reasons: the microphone hears the
speaker (ticket 12 of `tdeck-max` measured it at -35 dBFS), and the codec
runs both directions at one rate, so dictation's 16 kHz mono would play the
episode at the wrong speed. The player re-applies its format before playing
again, paused or not, since dictation changed it under it.

- [x] A capture notice the kernel keeps: dictation says when it takes the microphone and when it is done; a player listens
- [x] The podcast player pauses on the first, and on the second resumes only what it paused
- [x] The player sets its format again after any capture
- [ ] Verified on hardware: dictate over a playing episode, and it carries on at the right speed

## Comments

`rs_audio_capture_begin()` / `_end()` / `rs_audio_capturing()` in
`audio_volume.rs`, a count, with two tests. The player checks it beside the
call check it already had, and sets its format again after any pause, as it
did for a ringtone. It plays on when the recording ends, not when the
transcript is in: the microphone is free by then. A pause or play by the user
meanwhile cancels the playing on.

`test_podcasts.py`'s new `pauses_while_dictating` plays an episode, dictates
into Search, and checks the pause, the 16 kHz recording, the playing on and
the 44.1 kHz format set again. The pause and the recording's format change
race, in either order; up to one MP3 frame may play at 16 kHz in between,
which the microphone does not hear (it is still settling).
