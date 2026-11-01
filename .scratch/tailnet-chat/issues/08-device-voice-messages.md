# 08: Device: voice messages

**What to build:** in a conversation the user holds to record a voice message of up to 2 minutes, in Opus at 16 kHz mono, and sends it; it goes through the outbox in chunks and is acked once whole. A received one is kept on the card beside the conversation's history, shown with its length, and played when the user chooses. A player such as Podcasts pauses while one records or plays and resumes after. The device is held awake during a transfer.

**Blocked by:** 01, 04

**Status:** ready-for-agent

- [ ] Recording takes the audio as dictation does and stops at 2 minutes; under 300 ms is a tap, not a message.
- [ ] The file is Ogg/Opus as the protocol's vector is; it is kept as a `.opus` file beside the history and logged as a message that points to it.
- [ ] A transfer broken off is sent again from the beginning on the next connection; a half-received file is never shown.
- [ ] The whole recording is in PSRAM; the encoder's and decoder's stacks are as ticket 01 measured.
- [ ] The conformance suite's voice scenarios (whole and broken) pass against the simulator, which records from `THISTLE_SIM_MIC` and writes what it plays to a file the test reads.
