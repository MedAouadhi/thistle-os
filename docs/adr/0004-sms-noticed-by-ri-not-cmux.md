# SMS are noticed by the modem's RI line, not by multiplexing its UART

The A7682E keeps a received SMS in its own storage, but nothing read it. With mobile data off the modem was powered down. With it on, the PPP session holds the one UART, the module answers no AT commands, and the driver skipped SMS for as long as the session lasted, which is as long as mobile data stays on (ADR-0003). Only the Messenger, while its SMS chat was on screen, asked for them at all. We now keep the modem registered whenever SMS are wanted, with or without a data session. The network coordinator fetches what the modem holds whenever the driver says something may be waiting. During a session the driver says so only when the module pulls its RI line low (GPIO7 on the T-Deck Max, `"ri"` in the modem's board entry), which it does for 120 ms when an SMS arrives, whatever its UART is doing (A76XX Series Sleep Mode Application Note V1.03, 3.2). A fetch then leaves data mode with `+++`, reads and deletes, and redials, as sending an SMS already did.

## Considered Options

**CMUX** (AT+CMUX, 3GPP 27.010) splits the UART into a data channel and a command channel, and esp_modem supports it. Commands, URCs and SMS would flow alongside a live session, with no interruption at all. It was not chosen yet. It replaces the session handling ticket 10 tuned on hardware: escaping a session left over from a reset, the first dial after a power-on, the baud rate switch. None of that has been tried under CMUX. RI and the escape it triggers reuse the path that sending an SMS has already proven. CMUX remains the better end state, and nothing here stands in its way.

**Escaping the session on a timer** needs no RI line, but interrupts the session every few minutes to find nothing nearly every time, and still delays each SMS by up to the interval. It is kept only as the fallback for a board whose profile names no RI line: every ten minutes.

**Reading the SMS in the app** was how it worked, and is why nothing arrived: an app is closed most of the time. The kernel fetches them, records each in the message history (`msg_log.rs`), and queues them, as it does MeshCore's (ADR-0002).

## Consequences

`thistle_os.cellular.sms` (on unless switched off) keeps the modem powered and registered while mobile data is off: a few milliamps while the device is awake and the module idles. Switched off along with mobile data, the modem is powered down as before.

Each cellular attempt now registers first (`listen`), fetches, and only then dials, so a session is not interrupted the moment it starts. Switching mobile data off while SMS stay on powers the modem down and up again, the cleanest end to a session ticket 10 found.

Each incoming SMS during a session costs it a few seconds, as each outgoing one does. RI also rings for seconds at a time during an incoming call; after a fetch, RI is ignored in a session for 30 s.

Messages are read with `AT+CMGD=?` and one `AT+CMGR` each, not `AT+CMGL`. AT+CMGL marks every message it lists read, so one that did not fit its answer was never listed again. They are read under `AT+CSCS="UCS2"`, so non-ASCII text arrives intact. That makes an answer up to some 850 bytes, and esp_modem's DTE buffer is raised from 512 bytes to 1 KB to hold it. A message is deleted from the modem only once the kernel has it. A long message arrives in parts, which the kernel puts back together. Parts held for that are lost by a restart.

A board whose stored `board.json` predates the `"ri"` entry falls back to the ten-minute fetch during a session, until its profile is rewritten.
