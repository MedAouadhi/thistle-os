# 03: System volume and mute

**What to build:** The kernel keeps a volume level and a mute switch (`audio_volume.rs`, `thistle/audio_volume.h`), saves them as `thistle_os.audio.volume` and `thistle_os.audio.muted`, and gives the audio driver 0 while muted and the level otherwise: at boot and on every change. The `thistle_audio_set_volume` syscall goes through it.

**Blocked by:** none.

**Status:** done

- [x] `rs_audio_get_volume`, `rs_audio_set_volume(percent, save)`, `rs_audio_is_muted`, `rs_audio_set_muted`, `rs_audio_available`
- [x] Restored at boot
- [x] Syscall routed through it, unsaved
- [x] Host tests: effective level, reading the settings

## Comments

### Done

`audio_volume.rs`; restored after GPS at boot. The default volume is the ES8311 driver's own, 50. The quick panel's volume slider unmutes when raised above 0, as a phone's does.

### Confirmed

The user tried it on the T-Deck Max after the firmware above was flashed: it works.
