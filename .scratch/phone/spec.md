# Phone: voice calls and contacts

Status: done (issues/01)

Make and take voice calls on the T-Deck Max through its A7682E, and give it a
Phone app a smartphone user needs no explaining: contacts read from any
`contacts.vcf` on the SD card, a search that filters as you type, starred
contacts, recent calls, a keypad, and from any contact a Call or a Message.

## What the hardware does

All from the LilyGo vendor checkout (`~/personal/T-Deck-MAX/hardware/`).

- **The modem has its own microphone.** Schematic page 6, "MIC": a MEMS mic
  wired through ferrites straight to the A7682E's `MICP`/`MICN` (module pins
  9, 10). The ES8311 has a separate mic (page 8, MIC-4103, `MIC_P`/`MIC_N`).
- **The modem's audio output reaches the speaker through a switch.** Page 6,
  "SPK": `SPK1P`/`SPK1N` (pins 11, 12) through ferrites and 22 µF to
  `A7682E_OUTP`/`A7682E_OUTN`. Page 8: two RS2257 switches (U27, U28) pick
  `A7682E_OUT*` or `ES8311_OUT*` onto `OUTP`/`OUTN`, which feed the headphone
  jack directly and the NS4150B speaker amplifier through 1 kΩ. The select is
  `AUDIO_SEL`, XL9555 P12 (expander pin 10): **HIGH = A7682E, LOW = ES8311**
  (`docs/pinmap.md`, section 2). The amplifier's enable is XL9555 P06
  (expander pin 6), which the ES8311 driver already owns.
- **One output, an earpiece driver.** A7682E Hardware Design V1.05, 3.6: one
  MIC input, one SPK output (class AB, 32 Ω). There is no earpiece on the
  board, so a call is heard on the loudspeaker, or on headphones with the
  amplifier off.
- **RI stays low while a call rings.** Hardware Design 3.3.1: RI pulses low
  120 ms for an SMS, 60 ms for a URC, and for an incoming call "remains low
  until the host accepts the call using ATA, or the caller stops calling",
  whatever the UART is doing. RI is GPIO7, already a light- and deep-sleep
  wake source, and the power manager already keeps the device awake while
  it is low (`power_manager.rs`, `ringing()`).
- **The AT commands** (A76XX Series AT Command Manual V1.09, chapter 7 and
  2.2): `ATD<n>;` answers `OK` and later `VOICE CALL: BEGIN`; `ATA`;
  `AT+CHUP` (works whatever `AT+CVHU` says); `AT+CLCC` lists calls as
  `+CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,"<number>",<type>[,"<alpha>"]...]`
  with stat 0 active, 1 held, 2 dialing, 3 alerting, 4 incoming, 5 waiting,
  6 disconnect; `AT+VTS=<c>`; `AT+CMUT=0|1`; `AT+COUTGAIN=0..7` (default 4);
  `AT+CMICGAIN=0..7`; `AT+CSDVC=1` handset or `3` speaker phone. `RING`
  repeats every 6 s while a call rings.
- The driver sets no `AT+CNMP`, so the module stays in automatic mode and a
  call can fall back to 2G (CSFB) where the network has no VoLTE for it.

## Decisions

1. **The kernel follows calls by polling `AT+CLCC`, woken by RI.** No URC
   parsing: esp_modem's URC hook is compiled out (`CONFIG_ESP_MODEM_URC_HANDLER`)
   and would see nothing during a data session anyway. A kworker job reads
   RI's level once a second (`call_ringing`, a pin read). When a call rings,
   or the user dials, a call thread starts; it polls `AT+CLCC` every 500 ms
   while a call is up and exits once the call is over. No thread waits
   between calls.
2. **A call suspends the data session, as an SMS exchange already does**
   (ADR-0004). The driver leaves data mode for the first call command and
   resumes the session when `AT+CLCC` lists no call. Meanwhile the network
   coordinator dials nothing, fetches no SMS and powers nothing down, and a
   connect attempt under way when a call starts is cancelled.
3. **The audio driver owns the speaker path.** `hal_audio_driver_t.set_route`
   (appended) moves `AUDIO_SEL` and holds the amplifier for the voice route;
   the ES8311 driver gets `voice_route_expander`, `voice_route_expander_pin`
   and `voice_route_level` in its board entry. The modem driver never touches
   the speaker.
4. **The call interface is appended to `hal_net_driver_t`**, as SMS was:
   `call_dial`, `call_answer`, `call_hangup`, `call_list`, `call_ringing`,
   `call_dtmf`, `call_set_mute`, `call_set_volume`, `call_set_speaker`.
5. **Speaker on by default.** With no earpiece, a call with the amplifier
   off is silent without headphones. The call screen's Speaker button turns
   the amplifier off for headphones, and sets the modem's `AT+CSDVC` to
   handset (1) rather than speaker phone (3).
6. **The ringtone is synthesised** on the call thread and played through the
   ES8311 at the system volume, with the motor's `HAL_HAPTIC_RING` every two
   seconds. With the system muted it only vibrates.
7. **The in-call screen is system UI**, an overlay like the quick panel
   (`components/ui/src/call_ui.c`), so a call rings over whatever app is open
   without destroying it. Back, Home or Esc tuck an outgoing or connected
   call away to a chip in the status bar; a ringing call cannot be tucked
   away, and Back or Esc only silence it.
8. **The phonebook is the kernel's**, not the app's: the call screen, the
   Phone app, the Messenger and the call log all name numbers from it.
   `phonebook.rs`, with `thistle/phonebook.h`. It is separate from the
   Messenger's older `contact_manager.rs`, whose vCard import reads only bare
   `FN:` and `TEL:` lines.
9. **Stars and the call log live on the card**: `/sdcard/phone/starred.json`
   and `/sdcard/phone/calls.jsonl`. A `.vcf` is never written to.
10. **Calls and SMS hand over through the kernel.** Apps launch with no
    arguments, so "Message" on a contact sets `rs_sms_request_compose()` and
    launches the Messenger, which takes the request when it comes forward.

## Interfaces

The contracts every part is built against:

- `components/thistle_hal/include/hal/network.h`: `hal_call_t`,
  `hal_call_state_t`, and the nine call functions at the end of
  `hal_net_driver_t`.
- `components/thistle_hal/include/hal/audio.h`: `hal_audio_route_t`,
  `set_route`.
- `components/thistle_hal/include/hal/haptic.h`: `HAL_HAPTIC_RING`.
- `components/kernel/include/thistle/call.h`: calls and the call log.
- `components/kernel/include/thistle/phonebook.h`: contacts.
- `components/kernel/include/thistle/sms.h`: `rs_sms_request_compose()`,
  `rs_sms_take_compose()`.

## The Phone app

`apps/phone/`, id `com.thistle.phone`, "Phone". LVGL window managers only, as
every LVGL app. Four tabs along the bottom, as on a phone:

- **Favourites**: the starred contacts.
- **Recents**: calls, newest first. Each row a direction mark (outgoing,
  incoming, missed), the contact's name or the number, and when ("14:05",
  "Yesterday", "Mon", "12 Oct"). Missed calls stand out. Opening it marks
  missed calls seen. A Clear button, with a confirmation.
- **Contacts**: a search field on top, focused, so typing on the keyboard
  filters at once by name or by digits; the list below. An empty book says
  where to put a `contacts.vcf`, with a button to search the card again.
- **Keypad**: the number, the name it belongs to if any, 1-9 * 0 #, delete,
  and a Call button. The keyboard types into it; Enter calls; a long press on
  0 types "+".

Tapping a contact, a favourite or a recent call opens its page: the name, a
star to toggle, and each number with its label, a Call button and a Message
button. A number that is no contact gets the same page with the number as its
title. Lists are `lv_table`s and every row shares styles (CLAUDE.md, "LVGL's
pool is 64 KB"); at most 200 rows are filled at once, with a last row saying
how many more there are, which a search narrows down. Names are set in the
Reader's font (`reader_font_16`, `_mono` on e-paper) with its Arabic shaping,
as the Podcasts app does: contacts are not all ASCII.

## The call screen

Over everything but the status bar. Name large, the number under it, and
what is happening: "Incoming call", "Calling…", "Ringing…", the connected
time, "Call ended", "Missed call", "Call failed".

- Ringing here: **Decline** and **Answer**, and Silence.
- Placing or connected: **Mute**, **Keypad** (DTMF), **Speaker**, volume
  down and up, and **End call**.
- On e-paper the connected time is redrawn every 5 s, not every second.

The status bar shows a phone mark with the time of a call tucked away (a tap
brings the call screen back), and the number of missed calls (a tap opens the
Phone app).

## The simulator

`--call "NUMBER"` rings the fake modem with an incoming call a few seconds
after it registers; unanswered, it gives up after 20 s (missed). A dialled
number is answered after 3 s, except one ending in 0000, which is busy.
`--call-hangup SECONDS` has the far end hang up that long after connecting.
`simulator/sdcard/contacts/contacts.vcf` holds sample contacts in the
formats phones export, and `simulator/tests/test_phone.py` drives it all.

## Not done

- A second call while one is up (call waiting, hold, conference): a waiting
  call is left to ring out; the modem's network may send it to voicemail.
- Editing or adding contacts on the device.
- Calls under the thistle-tk window manager.
- Choosing a ringtone.

## What the hardware has to confirm

- That `AT+CLCC` lists a ringing call, with its number, during and after a
  data session the call interrupted, and that RI stays low until `ATA`.
- That the network carries the call: VoLTE, or a fall back to 2G.
- That `AUDIO_SEL` high puts the far end on the speaker, and the MEMS mic
  reaches the far end; whether `AT+CSDVC=3` keeps the far end free of echo.
- The right `AT+COUTGAIN` for the NS4150B: loud enough, not distorted.
