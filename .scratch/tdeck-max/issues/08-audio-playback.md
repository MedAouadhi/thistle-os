# 08: Audio playback on the T-Deck Max

**What to build:** Apps can make sound through the speaker, and the board is silent when nothing is playing.

The codec is new to ThistleOS and is wrapped around the maintained first-party ESP-IDF codec component rather than reimplementing a long register initialisation sequence. This follows the existing precedent for the two C drivers already in the tree, both cases where a mature vendor library was preferred to a port.

This ticket also introduces the runtime expander setter described in ADR-0001. The speaker amplifier's enable line sits on the IO expander, and it cannot be a static boot-time setting: left permanently enabled the board hisses and draws current at idle. The audio driver owns that line and gates it around playback. The setter is deliberately minimal — one call through the syscall table, not a HAL interface — and is the seam a full IO-expander interface will grow from when the 4G modem is implemented.

The audio multiplexer on this board can route the speaker to either the codec or the cellular modem. With the modem out of scope, it is set statically to the codec.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** in-review

- [x] A runtime expander pin setter is available to drivers through the syscall table
- [x] A codec driver registers the audio HAL interface, wrapping the first-party codec component
- [x] The audio multiplexer is set to the codec route
- [x] Playback is audible through the speaker
- [x] The amplifier is enabled only while audio is playing; the board is silent and not drawing amplifier current at idle
- [x] A TODO is recorded that audio capture has no HAL path, so the codec's microphone input is unreachable — the same shape of gap as haptics

## Comments

### The data line: LilyGo's pinmap is wrong, and their examples are right

`docs/pinmap.md` §12 gives "ESP32 DOUT -> ES8311 DSDIN" as **GPIO17**. The
schematic (page 8, the pin table under the codec) has `I2S_DSDIN ------ IO40`
and `I2S_ASDOUT ------ IO17`, so the SoC's data out is **GPIO40**. The vendor
header has the two macro values swapped relative to the schematic
(`BOARD_ES8311_ASDOUT 40`, `BOARD_ES8311_DSDIN 17`), and their working sketches
pass `BOARD_ES8311_ASDOUT` as `data_out` (`examples/ES8311/playWAV/playWAV.ino`,
`setPins(MCLK, SCLK, LRCK, ASDOUT, DSDIN)`), which comes out as 40. The
schematic and the sketches agree, and the pinmap row is the one that's wrong.
The profile uses `i2s_data: 40`, and a board-config test records why.

### Where each value came from

| Field | Value | Source |
| --- | --- | --- |
| Codec address | 0x18 | pinmap §12/§13, schematic p8 "I2C Address:0x18" |
| MCLK / BCLK / LRCK | 38 / 39 / 18 | schematic p8 pin table, `TDeckMaxBoard.h` |
| SoC data out | 40 | schematic p8 `I2S_DSDIN`, see above |
| Amplifier enable | expander pin 6, high = on | `BOARD_XL9555_06_AMPLIFIER (6)` |
| Speaker mux | expander pin 10, low = ES8311 | `BOARD_XL9555_12_AUDIO_SEL (10)` |

The mux is named P12 (port 1, bit 2) but its linear number is **10**. Writing
12 would drive P14, which is not connected.

### Why the amplifier line has to be claimed

The NS4150B's enable has a 10K pull-up on the schematic, and the expander
resets every pin to an input. So the amplifier is **on** from power-on until
something drives the line, which is the idle hiss the ticket describes. The
line is now held low from two places, deliberately:

- the board profile's expander phase drives it low at boot, so the board is
  quiet from the earliest moment, well before drivers load;
- the driver drives it low again in `init` before it touches the codec. That
  means the driver never depends on the profile (ADR-0001), and a codec that
  fails to come up still leaves the speaker silent.

After that the driver owns the line. `play` opens the codec, applies the
volume, then raises the amplifier, so codec start-up isn't amplified. `stop`
drops it before closing the codec. A caller that never calls `stop` is covered
by a 500 ms idle timeout. The timeout takes the driver's lock with a zero wait
and stands down if a play is in progress; that play re-arms it on the way out.

### The setter: `thistle_expander_set_level(expander, pin, level)`

Driver-only in the syscall table (apps cannot resolve it), declared in
`kernel_rs/include/thistle_expander.h` for compiled-in drivers and in the
driver SDK for loadable ones. It makes the pin an output if the profile did
not, and it **latches the level before switching direction**, the reverse of
the boot phase. The latch resets high, so direction-first would pulse the
amplifier on for one I²C transaction on its way to off. A line that is already
an output is not reconfigured, so gating costs one register write per edge.

### Verified on hardware

With the storage partition erased so the new profile could reach the board
(the trap tickets 03 and 06 recorded), a temporary boot probe played two 1.5 s
1 kHz tones through the HAL: one ended by the idle timeout, one by `stop`.

```
board_builtin: audio-es8311: ... addr=0x18 i2s=0 mclk=38 bck=39 ws=18 data=40 amp_exp=0/6
es8311: PROBE amp OFF at 1160 ms
es8311: initialized on I2S0 (MCLK=38 BCLK=39 WS=18 DOUT=40) amp=0/6
es8311: PROBE amp ON at 9636 ms
probe: round 0: 64 chunks in 1512 ms, last=ESP_OK
es8311: PROBE amp OFF at 11611 ms          <- 500 ms after the last write
es8311: PROBE amp ON at 12648 ms
probe: round 1: 64 chunks in 1510 ms, last=ESP_OK
es8311: PROBE amp OFF at 14123 ms          <- stop()
```

64 × 1024 frames is 1486 ms of audio at 44.1 kHz, delivered in 1512 ms, so
I²S is clocking in real time. The user heard two clean tones, then silence.
The probe was removed and the clean build reflashed.

Not measured: amplifier supply current at idle. "Silent at idle" is by ear and
from the enable line's logged state, not from a meter.

### Known noise

Each codec open logs
`E i2s_common: i2s_channel_disable(1218): the channel has not been enabled yet`.
This is `esp_codec_dev` disabling the channel before it reconfigures the clock,
and it's harmless. Silencing it would mean either filtering `i2s_common` logs,
which would hide real errors, or keeping I²S clocked while closed. Left as is.

### Follow-up: no app plays audio yet

The HAL path works, but nothing in the tree calls `audio->play`, and the
syscall table has no app-facing audio call. The ticket's "apps can make sound"
is true for code that holds the registry (drivers, kernel, built-in C apps).
A signed ELF app has no way to reach it. That needs an app ABI decision and
belongs in its own ticket.

### TODO: audio capture

Recorded in `drv_audio_es8311.h`. The codec's ADC and the board's microphone
are unreachable because the audio HAL has no capture path. Same shape of gap as
haptics: a kernel ABI change of its own.

### Code review fixes

The two-axis review found three real defects. Each got a failing test first:

- **The amplifier could be left on for good.** The idle timeout stood down
  whenever it could not take the lock, on the assumption that only a `play`
  holds it and a `play` re-arms the timer. `set_volume` holds it too and
  re-arms nothing. The timeout now tries again when it finds the driver busy.
- **A late timeout after `deinit` touched a freed mutex.** `esp_timer_stop`
  does not wait for a callback already on its way. The lock and timer are now
  created by the first `init` and never deleted, and the callback checks
  `initialized` under the lock.
- **The expander's shadow registers recorded writes the part never took**, so a
  failed write leaked a wrong level into the next write to that port. This
  affected the boot phase's `set_level` / `set_as_output` too. The three
  callers now share `write_output` / `make_output`, which update the shadow
  only on success.

The final build was flashed and boots with `es8311: initialized`. The
re-verification is of init only: the tone probe was not re-run after these
fixes, which touch the timeout and lifetime paths that the host tests cover.
