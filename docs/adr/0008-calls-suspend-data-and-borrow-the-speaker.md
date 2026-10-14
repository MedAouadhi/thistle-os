# A call suspends the data session, and borrows the speaker from the audio driver

The T-Deck Max can make voice calls: its A7682E has a MEMS microphone of its own (schematic page 6, "MIC", on the module's `MICP`/`MICN`), and its one audio output, `SPK1P`/`SPK1N`, reaches the speaker amplifier and the headphone jack through an analog switch that `AUDIO_SEL` (XL9555 P12) turns from the ES8311 to the modem (page 8, U27 and U28; T-Deck-MAX `docs/pinmap.md`, section 2). Two things had to be decided: how the kernel talks to the modem about a call while a PPP session holds the modem's one UART, and who switches the speaker.

**The modem.** A call is followed by asking the modem for its calls, `AT+CLCC`, every 500 ms while one is up, and that needs command mode. The first call command, or the first look at a call ringing, leaves a data session the way an SMS exchange does (ADR-0004), and the session is redialled once `AT+CLCC` lists no call. In between the network coordinator dials nothing, fetches no SMS and powers nothing down. A ringing call is noticed by the RI line, which the A7682E holds low from the first ring until the call is answered or given up (A7682E Hardware Design V1.05, 3.3.1). A job on the kernel worker reads its level once a second, and a call thread exists only while a call does.

**The speaker.** The audio HAL gained `set_route` (appended, so no field moved), and the ES8311 driver, which already owns the amplifier's enable line, now owns `AUDIO_SEL` too. For a call the kernel asks it for the voice route, with the amplifier on or off. Meanwhile the driver refuses playback and keeps its idle timer from dropping the amplifier.

## Considered Options

**CMUX** would let commands flow beside a live session, so a call would not cost mobile data its session. ADR-0004 explains why it waits: it replaces session handling tuned on hardware. A call interrupts data for its length, which a phone on 2G does as well.

**Reading `RING`, `+CLIP` and `VOICE CALL:` as they arrive** needs esp_modem's URC hook (`CONFIG_ESP_MODEM_URC_HANDLER`), which hands over raw chunks of the UART stream, and would hear nothing during a data session anyway. Polling `AT+CLCC` needs neither, and says the state of every call in one answer.

**The modem driver switching the speaker itself** would put two drivers on the amplifier's enable line: the ES8311's idle timer would cut a call's audio half a second after any sound it played.

## Consequences

Each call costs a data session its connection for the length of the call and a few seconds either side, as each SMS already does.

The device has no earpiece, so a call starts on the loudspeaker, and the modem is set for a speaker phone (`AT+CSDVC=3`). The call screen's Speaker button turns the amplifier off for headphones and sets the modem's handset mode.

A board whose stored `board.json` predates the ES8311's `voice_route_*` keys makes calls nobody hears until its profile is rewritten, as for `"ri"` (ADR-0004).
