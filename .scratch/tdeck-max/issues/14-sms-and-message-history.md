# 14: Receive SMS, and keep every message on the SD card

**What to build:** An SMS sent to the T-Deck Max's SIM arrives, whatever app is open and whether or not mobile data is on. Every SMS and MeshCore message, received or sent, is saved to the SD card unless the user switches that off in the Messenger.

**Blocked by:** 10 (cellular data), 13 (sleep, for the modem's RI line).

**Status:** in-review

- [x] The modem stays registered for SMS with mobile data off (`thistle_os.cellular.sms`, on unless switched off; Settings → Mobile data → Receive SMS)
- [x] The kernel fetches SMS on the network coordinator's thread, Messenger open or not, and queues them (`sms_manager.rs`, `thistle/sms.h`)
- [x] During a data session an SMS is noticed by the modem's RI line (GPIO7, `"ri"` in the modem's board entry), and fetched by interrupting the session (ADR-0004)
- [x] No message is lost to AT+CMGL's answer filling up: each is read on its own, and deleted from the modem only once the kernel has it
- [x] Text arrives intact: read under AT+CSCS="UCS2", decoded by the data coding scheme (GSM 7-bit, 8-bit, UCS2); a long message's parts are put back together
- [x] One Messenger conversation per phone number, which can be replied to; a number can be typed to start one
- [x] Every SMS and MeshCore message, in or out, is appended to `/sdcard/messages/{sms,mesh}/<conversation>.jsonl` (`msg_log.rs`, `thistle/msg_log.h`)
- [x] The Messenger's settings (gear in its header) switch saving off and on (`thistle_os.messenger.save_history`, on by default)
- [x] The status bar's envelope counts unread SMS as well as mesh messages, waiting in the kernel or held unread by the Messenger (`rs_sms_unread_count`)
- [x] Verified on hardware: an SMS received and one sent on the T-Deck Max, with mobile data on
- [ ] The Messenger shows the saved history after a restart. Not done: it is saved, not read back.

## Comments

### Why the SMS from a web platform never arrived

Four things, any one of which was enough:

1. **With mobile data off, the modem was powered down.** ADR-0003 said so: "in practice no SMS is received at all".
2. **With mobile data on, the PPP session holds the UART,** and the driver's poll skipped SMS for as long as a session was up, which is as long as mobile data stays on.
3. **Only the Messenger's SMS chat asked for SMS,** and only while it was on screen: `start_receive` on opening it, `stop_receive` on leaving it or hiding the app.
4. **The poll lost messages.** `AT+CMGL="REC UNREAD"` marks every message it lists read (A76XX AT Command Manual V1.09, 9.2.11). Its answer went into a 512-byte buffer, which holds about two messages. Any that did not fit were marked read anyway, and a poll for unread ones never listed them again.

An SMS that arrived while the modem was off waits at the service centre for its validity period, often one to seven days, and is delivered once the modem registers. One sent recently may still arrive once this is on the board.

### On the hardware

Flashed to the T-Deck Max on /dev/ttyACM0. On the first boot, with the old `board.json` (`RI=-1`), the modem registered on MEDIONmobile at -61 dBm and fetched an SMS that had waited for it (`sms: 1 received`) before dialling. With `board.json` rewritten (`rm /spiffs/config/board.json` in the Terminal, then a reboot; the driver logs `RI=7`), an SMS was received with mobile data on and one was sent. The status bar did not count it until the envelope learned SMS, after which a second flash booted to `ThistleOS ready; internal RAM 149 KB free`.

What was first written here, before any of it ran on a T-Deck Max: In the simulator, a fake modem took the coordinator through register, fetch and dial, and the Messenger through receive, reply and the settings switch. The A7682E has yet to confirm:

- **AT+CMGD=? lists the stored messages' indices.** The manual's example (`+CMGD: (1),(0-4)` with one stored) says so. If it lists every slot instead, the fetch reads up to 32 slots and stops at the count AT+CPMS? gave, which still works, only more slowly.
- **RI falls during a data session when an SMS arrives.** The Sleep Mode Application Note (3.2) describes RI with no mention of the UART's mode. The log shows `sms: N received` after a fetch.
- **Text under AT+CSCS="UCS2"** comes as 27.005 3.1 describes, and AT+CSDH=1 adds `<fo>` and `<dcs>` to the header.
- **esp_modem takes a 1 KB DTE buffer** (`dte_buffer_size`), needed for an SMS read as UCS2.

To try it: switch mobile data off, leave Receive SMS on, send an SMS to the SIM, and look for `cellular: registered`, then `sms: 1 received`, then a file under `/sdcard/messages/sms/`. Then switch mobile data on and send another. The modem's RI line should bring it in within a few seconds, with `PPP session` lines either side as the session is interrupted and resumed.

### The board profile

`"ri": 7` is new in the modem's entry. A board that already holds a `board.json` keeps its old one (CLAUDE.md, "A changed board profile does not reach a board that has one"). Without `ri`, an SMS in a data session is fetched every ten minutes, and with no session every 15 s, so SMS still arrive, only later. Rewrite `/spiffs/config/board.json` as for tickets 03-10.

### The history files

```
/sdcard/messages/sms/+4917612345678.jsonl
/sdcard/messages/mesh/public.jsonl
/sdcard/messages/mesh/1a2b3c4d5e6f.jsonl
```

```
{"dir":"in","from":"+4917612345678","sent":1760351696,"text":"Are you there?","time":1791887751}
{"dir":"out","text":"Yes, here!","time":1791887757,"to":"+4917612345678"}
```

They are plain text on a removable card. An SMS that came encrypted by `msg_crypto` is saved as it arrived, still encrypted; the Messenger decrypts only for display.

### Left as they were

- The Messenger's Montserrat font has no glyphs beyond ASCII, so "ü" or "€" in a message shows as a box, though it is stored correctly.
- Sending an SMS blocks the LVGL task for the exchange, as it did before; seconds with a data session to interrupt.
- Outgoing SMS are plain ASCII, up to 160 characters, as the driver's validation allows.
