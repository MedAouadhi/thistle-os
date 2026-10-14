# 01: Voice calls, and a Phone app with contacts from the SD card

Status: resolved
Type: task

The spec is `../spec.md`, the decision ADR-0008.

- [x] The A7682E places, answers and ends calls, lists them (`AT+CLCC`), and sends DTMF, mute, volume and speaker settings: nine functions appended to `hal_net_driver_t` (`a7682e_call.c`). A call holds the line: the first call command leaves a data session and `call_list` resumes it once no call is listed
- [x] A call ringing is noticed by RI held low for 250 ms, a pin read (`call_ringing`)
- [x] The ES8311 driver owns the speaker switch: `set_route` (appended to `hal_audio_driver_t`), with `voice_route_expander`, `voice_route_expander_pin` and `voice_route_level` in the T-Deck Max's ES8311 entry (XL9555 P12, expander pin 10, HIGH = modem)
- [x] The kernel's call manager (`call_manager.rs`): a 1 s kworker job watches RI; a call thread, only while a call is up, polls the modem every 500 ms, rings (440 + 480 Hz at the system volume) and buzzes (`HAL_HAPTIC_RING`, DRV2605 effect 16), routes the speaker on connecting, holds the device awake, and keeps the network coordinator off the modem
- [x] The call log, `/sdcard/phone/calls.jsonl`, and the missed-call count (`call_log.rs`)
- [x] The phonebook (`phonebook.rs`): every `contacts.vcf` on the card down to 4 folders, vCard 2.1, 3.0 and 4.0 as Android, iOS and Google export them; search by name or digits; numbers matched by their last 9 digits; stars in `/sdcard/phone/starred.json`
- [x] The call screen, system UI over any app (`call_ui.c`): Answer, Decline and Silence; Mute, Keypad, Speaker, volume and End call; Back tucks a call away into the status bar, which also counts missed calls
- [x] The Phone app (`apps/phone/`): Favourites, Recents, Contacts with search as you type, Keypad, and a page per contact or number with Call and Message
- [x] "Message" opens the Messenger's conversation with the number (`rs_sms_request_compose`), an existing one if the last 9 digits match; SMS conversations are named from the phonebook
- [x] A podcast pauses while a call is up
- [x] Settings → Mobile data's "Receive SMS" is now "Calls and SMS", and its "SMS only" "Calls & SMS"
- [x] The simulator's fake modem rings, answers, dials, is busy and hangs up (`--call`, `--call-hangup`, `--call-preview`); `test_calls.py` and `test_phone_app.py`
- [x] Verified on hardware: a call, by the user's report
- [ ] Verified on hardware: the details listed below

## Notes

### On the hardware

Flashed to the T-Deck Max on /dev/ttyACM0. With the old `board.json` the ES8311 logged `voice_route=-1/0@1`; with it rewritten, `voice_route=0/10@1`. Boot ends at `ThistleOS ready; internal RAM 150 KB free, largest block 74 KB`, as before.

The first build crashed the moment the Phone app opened: `***ERROR*** A stack overflow in task phonebook has been detected`. The phonebook's reader had 8 KB, and searched the card folder within folder; every look at the card goes down through newlib, the VFS, FatFs and the SD card's SPI driver. The search is now a loop over a list of folders, the reader has 16 KB (it exists only while reading) and warns if it leaves under 4 KB unused, and the call thread, which writes the call log, has 12 KB. Rebuilt, the app opened, searched the card (`no contacts.vcf on the card`: there was none yet) and moved between every tab.

The user then made a call, which worked. The serial capture had lost the port to light sleep by then, so there is no log of it. Still to see in a log or by ear:

- That `AT+CLCC` lists a ringing call with its number, during a data session and after one the call interrupted, and that the session comes back after the call (`call: no calls; data session resumed`).
- Both directions: an incoming call through RI, from light sleep too, and an outgoing one.
- That the far end hears no echo with `AT+CSDVC=3`, and that `AT+COUTGAIN` at the default 70 % (gain 5 of 7) is neither faint nor distorted through the NS4150B.
- That `call_ringing` does not take an SMS for a call.
- The phonebook's stack: a card with many folders and a large `contacts.vcf` should not log `reading left N of 16384 bytes of stack unused`.

### Tests

Rust kernel 1878 (64 new). Host: call arguments, modem answer parsing (`+CLCC`), the ES8311's route, the Phone app's text helpers, board bus init, board catalog. Simulator: calls 24, Phone app 24, app matrix 114, launcher visuals 9, quick panel 10, fits screen 7, touch taps, keyboard typing, Notes, App Store, Podcasts, Assistant: all pass, and the simulator's SD card is left as it was. `test_launcher_visuals.py`'s two app-grid hashes changed for the Phone tile, the only difference in the captures.

`test_epaper_partial_flush.py`'s `eink-settings-values` fails as it did before this ticket (ticket quick-panel 05): the Mobile data value moves from "Starting" to registered between its two captures. Its first capture showed "Calls and SMS only" squeezing "Mobile data" onto two lines on 240 px, so the value is now "Calls & SMS".

### Known

- A second call while one is up is left to ring out.
- The Reader's font, which the Phone app sets names in, draws a gap after "m" ("Emm a"); the Reader shows the same.
- Phonebook indices move when the book is read again; the app finds a contact again by name and first number.
