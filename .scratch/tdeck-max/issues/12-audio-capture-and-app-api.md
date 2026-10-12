# 12: Audio capture, and audio for apps

**What to build:** Apps can play through the speaker and record from the microphone, through syscalls, as one service several apps can share. Ticket 08 left two gaps. The audio HAL had no capture path, so the microphone was unreachable. And nothing outside the kernel could reach audio at all, because the syscall table had no app-facing audio call.

**Blocked by:** 08 (audio playback on the T-Deck Max).

**Status:** in-review

- [x] The audio HAL has a capture path: `record` and `set_mic_gain`, appended after `name` as `set_backlight` was on the input HAL
- [x] The ES8311 driver records over a duplex I2S pair, with the codec in DAC + ADC mode
- [x] The board profile names the microphone's data line (`i2s_din`)
- [x] Apps reach audio through six `thistle_audio_*` syscalls, gated by the existing and until now unused `audio` permission
- [x] Recording leaves the speaker amplifier off
- [x] A recording starts clean, without the microphone's power-up transient
- [x] A player and a recorder in different tasks run at once without holding each other up
- [x] Verified on hardware
- [ ] Settings still reads "Volume: 100%" and has no volume control. Out of scope here.

## Comments

### The microphone's data line is GPIO17

The schematic's pin table (page 8) has `I2S_ASDOUT ------ IO17`: the codec's ADC output, which is the SoC's data in. LilyGo's `docs/pinmap.md` gives this row as GPIO40, the same swap ticket 08 found on the data-out row. Their sketches only ever play, so nothing in their tree exercises the line. The profile now has `"i2s_din": 17`, and the board-config test asserts it next to `i2s_data: 40`.

A profile without `i2s_din` gets playback alone, and `record` answers `ESP_ERR_NOT_SUPPORTED`.

### What an app sees

```c
int thistle_audio_configure(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
int thistle_audio_play(const uint8_t *data, size_t len);
int thistle_audio_record(uint8_t *buf, size_t len);
int thistle_audio_stop(void);
int thistle_audio_set_volume(uint8_t percent);
int thistle_audio_set_mic_gain(uint8_t db);
```

All six need the `audio` permission. Each returns an `esp_err_t`. A board with no audio driver, or a driver with no input side such as the PCM5102A, answers `ESP_ERR_NOT_SUPPORTED`. A buffer is refused with `ESP_ERR_INVALID_ARG` unless both its first and last byte lie in memory an app may hand the kernel. The existing calls check only the first byte, which would let a long `len` run on into internal RAM. `configure` takes scalars rather than a struct, so an app need not place one in PSRAM.

Playback and capture share one format, because the codec runs both from one word clock. The microphone is mono and appears in both channels of a stereo recording.

### What the hardware showed

Three things changed on the way, each found by a temporary probe build that played a 1 kHz tone at -12 dBFS and measured what the microphone heard.

**The right channel of a recording was the playback.** By default `esp_codec_dev` has the codec put the DAC's input on the right slot of ASDOUT, as an echo-cancellation reference (register 0x44, `ADCDAT_SEL` 5). The first probe's right channel came back as exactly ±8191, the tone as written. An app recording in stereo would have had whatever was playing in its file. The driver now sets `no_dac_ref`, which writes `ADCDAT_SEL` 0. The library's comment says that leaves the right slot empty. The ES8311 user guide calls it "ADC + ADC", and on the board both channels came back identical, so the guide is right.

**Every recording started with a thump.** For the first ~75 ms after the codec opens, the input swings full scale. A DC offset of a few thousand counts then decays into the noise floor by about 400 ms. The probe measured it in 25 ms steps at 8, 16, 44.1 and 48 kHz, twice each. It lasts the same time at every rate, so it is analog, most likely the RC network on the mic bias. The driver now drops the audio captured in the first 400 ms after an open. If playback opened the codec earlier, it drops only what is left of that window in the I2S ring. A recording that starts the codec takes about 400 ms longer to begin, and a recorder that keeps reading pays this once.

**One lock across both directions.** As first written, the driver held its single lock across each write and each read. A recorder in one task and a player in another would then wait for each other's I/O. A player that gives the mutex back and takes it again at once can starve a waiting recorder, whose 1440-frame DMA ring then overflows. That was reasoned from FreeRTOS mutex behaviour, not observed. The fix was then measured. Playback and capture each have a lock held across their I/O, and a third lock covers the codec's state for short sections only. Closing the codec takes all three, playback first, then capture, then state, so it waits for both directions to finish.

### Verified on hardware

T-Deck Max on /dev/ttyACM0. The board held a `board.json` from before this ticket, the trap tickets 03, 06, 09 and 10 met. A temporary build re-ran detection once and rewrote only `/spiffs/config/board.json`: `board_detect: Wrote the detected board's profile to /spiffs/config/board.json`, then `audio-es8311: ... data=40 din=17`.

Final driver, single task, mic gain 24 dB, volume 50:

| | 44.1 kHz stereo | 16 kHz mono |
| --- | --- | --- |
| Quiet room, RMS | -44.3 dBFS | -42.8 dBFS |
| Speaker tone heard by the mic, 1 kHz bin | -35.4 dBFS | -35.4 dBFS |
| Same, amplifier held off | -94.6 dBFS | -100.9 dBFS |
| 1.5 kHz bin during the tone | -111.0 dBFS | -107.1 dBFS |
| Clipped samples | 0 | 0 |

Holding the amplifier off drops the tone by 59 and 65 dB. So the microphone hears the speaker through the air, not by crosstalk on the board, and the speaker makes sound. With the settling drop in place, every 25 ms segment from a recording's first sample was at the noise floor, at all four rates.

The amplifier was off while recording alone (`amp=0 open=1`), on during playback, and off after `stop`. One read followed by nothing closed the codec within 700 ms.

Two tasks at once, 44.1 kHz stereo, the player starting 50 ms ahead:

| Player writes | Player, 3 s of audio | Recorder, 2 s + 0.4 s settling | Mic's 1 kHz level per 100 ms |
| --- | --- | --- | --- |
| 441 frames | 3040 ms | 2392 ms | -35.7 to -35.6 dBFS |
| 4410 frames (3x the ring) | 3029 ms | 2436 ms | -35.3 dBFS throughout |

The player kept real time through the recorder's settling, and the recorder kept real time through 100 ms writes. A first run of the 4410-frame case showed the player underrunning. That was the probe's fault. It computed each 100 ms of tone with double-precision `sin()`, about 84 ms of work per chunk against a 33 ms ring. A precomputed tone fixed it.

The probes were removed and the clean build flashed. It boots with `es8311: initialized on I2S0 (MCLK=38 BCLK=39 WS=18 DOUT=40 DIN=17) amp=0/6`.

### Internal RAM

The two I2S channels' DMA rings and handles take 12,548 bytes of internal RAM, measured around `port_codec_create`. Ticket 11 had playback's alone at 6.6 KB, so capture adds about 6 KB. The clean build boots with `internal RAM 159 KB free, largest block 80 KB`. The firmware on the board before this ticket reported 166 KB, but that was an older build (`v0.5.2-141-ga730e27-dirty`), so the two numbers are not a clean comparison.

Creating the I2S channels only while the codec is open would free all 12.5 KB at idle. Not done here.

### Tests

- `drv_audio_es8311/tests`: 30 host tests under ASan and UBSan, 14 of them new. They cover record leaving the amplifier off, the gain applied before the first read, the settling drop in its three cases (the record opens the codec, playback opened it long ago, playback opened it moments ago), and I/O holding only its own direction's lock.
- `kernel_rs`: 1658 tests. New ones check that the six syscalls resolve only with the `audio` permission, answer `ESP_ERR_NOT_SUPPORTED` with no driver or no input side, reach the driver with the right arguments, and refuse null, empty and wrapping buffers.
- The simulator's audio driver returns silence from `record`.

### Known noise

Each codec open now logs `E i2s_common: i2s_channel_disable(1218): the channel has not been enabled yet` twice, once per channel. Ticket 08 explains why it is harmless.

### Still open

- Boards that already hold a `board.json` get no microphone until it is rewritten. `record` answers `ESP_ERR_NOT_SUPPORTED` meanwhile.
- No app records or plays yet.
- Speech has not been tried. The evidence is the speaker loopback and the room's noise floor, and nobody has listened to a recording.
