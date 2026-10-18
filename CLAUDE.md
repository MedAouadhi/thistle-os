# ThistleOS Development Guide

## Project Overview
ThistleOS is a portable ESP32 family operating system with an immutable kernel, loadable drivers and apps, swappable window managers, and Ed25519 signing at every level. Supports multiple boards across ESP32, S2, S3, C3, and C6 architectures. Targets LilyGo T-Deck Pro (e-paper), T-Deck (LCD), T-Display-S3, T3-S3, CYD, and C3-Mini.

## Hardware authority: the LilyGo vendor repo

`~/personal/T-Deck-MAX/` is a checkout of
`github.com:Xinyuan-LilyGO/T-Deck-MAX` — the board manufacturer's own
firmware. **It is the authority on T-Deck (Pro / Max) hardware.** When a pin,
an I2C address, a part number, a reset line, or a register map is in question,
answer it from there before writing driver code, and cite what you used.

Do not arbitrate between unrelated vendor libraries found elsewhere on disk or
online. Reasoning from a plausible-looking driver for a *different* part has
repeatedly produced wrong register maps, and once hung the I2C bus hard enough
at init to trip the interrupt watchdog and boot-loop the board.

What is in there, in descending order of authority:

- `hardware/` — the **schematics and the chip datasheets**, as PDFs. The board
  schematic is `hardware/T-Deck Pro Max V0.1 25-09-11/*.pdf`; alongside it sit
  the datasheets for the parts actually fitted (`touch-CST328_V2.2.pdf`,
  `MIA-M10Q_DataSheet_*.pdf`, `GDEQ031T10.pdf`, `UC8253.pdf`, `tca8418.pdf`,
  `XL9535_*.pdf`, `ES8311.*`, `drv2605.pdf`, `S62F_Lora.pdf`, the A7682E
  manuals). Read these with the `pages` argument of the Read tool.
- `docs/pinmap.md` (English; `pinmap_cn.md` is the same in Chinese) — the
  schematic transcribed per module, each row citing its schematic page and
  cross-checked against the repo's macros. This is the fastest correct answer
  for "which pin is X on". It is what establishes, for example, that touch is a
  **CST328 at 0x1A**, INT on GPIO12, with reset on **XL9555 P07** rather than a
  direct GPIO, and that `GPS_EN` is **XL9555 P02**.
- `lib/TDeckMaxBoard/src/TDeckMaxBoard.h` — the pin macros the vendor firmware
  compiles against.
- `lib/` — the vendor's driver libraries for the fitted parts, notably
  `HynTouch/` (`hyn_cst3xx.c` covers the CST328), `SensorLib/`, `XPowersLib/`,
  `GxEPD2/`, `RadioLib/`, `Adafruit TCA8418/`, `TinyGPSPlus/`.
- `examples/` — working per-peripheral sketches, including `touch_hyn_core/`,
  `test_GPS/`, `XL9555/`, `keypad/`, `Elink_paper/`, and the `factory/` image.
- `firmware/*.bin` — a prebuilt factory image, if a board ever needs to be
  proven good against known-working vendor firmware.

Note the naming trap the vendor doc calls out: schematic net names are written
from the *peripheral's* point of view (`GPS_TX`, `7682_RXD`, `I2S_DSDIN`), so
TX/RX are crossed relative to the SoC pin that drives them.

## Build System
- **Firmware**: ESP-IDF v5.5 with CMake. `idf.py build`, `idf.py flash monitor`
- **Simulator**: SDL2 desktop build. `cd simulator/build && cmake .. && make -j8 && ./thistle_sim`
- **Rust kernel**: `cargo +esp check` in `components/kernel_rs/`. Tests: `cargo test --target <host-triple> -- --test-threads=1` (1878 tests)
- **Recovery OS**: `cd recovery && cargo +esp build --release`
- **CI**: GitHub Actions — firmware build (espressif/idf:v5.5), Semgrep SAST, Trivy security scan

### Sharing one USB-Serial-JTAG board between sessions

A `/dev/ttyACM*` port hands each byte to exactly one reader. If another
process (typically a capture loop left running by another agent session) has
the port open, `idf.py flash` dies with `The chip stopped responding` or
`device reports readiness to read but returned no data (... multiple access on
port?)`, and console captures come back empty or partial. This has cost days
on the T-Deck Max and was misdiagnosed as a hardware fault. Before `idf.py
flash`, `idf.py monitor`, or any capture, run:

```sh
tools/check_serial_port.sh /dev/ttyACM0
```

It exits non-zero and lists the holders (PID, cwd, command) if the port is
taken. Stop them first; never start a second reader on the same port.

With the port to yourself, `idf.py -p /dev/ttyACM0 flash monitor` works on the
T-Deck Max exactly as on any other board: a full 2.4MB app flashes in about
21 seconds at the default settings, and `idf.py monitor` shows the whole boot
including the `cpu_start` region. The chunked flasher in the T-Deck-MAX
vendor checkout is not needed. See `docs/tdeck-max-usb-serial-jtag.md`.

### A changed board profile does not reach a board that has one

A board writes its profile to `/spiffs/config/board.json` once, from the
embedded copy in `components/kernel/src/board_fingerprint.c`, and reads that
file from then on. The firmware build makes no SPIFFS image, so flashing a
changed profile changes nothing on a board that already holds one: tickets 03,
06, 09 and 10 all met this. Erasing the storage partition fixes it but also
wipes `system.json` (WiFi credentials, settings) and the mesh identity.
Rewriting only `board.json`, for instance by a build that calls
`board_detect_and_write()` once, keeps them.

### Internal RAM runs out first; keep stacks there and buffers out

An S3 has about 340 KB of internal RAM, shared with any code placed in IRAM,
against 8 MB of PSRAM. A T-Deck Max once booted with 14 KB of internal RAM
left, where a 4 KB task could not be created (ticket 11); it now has about
150 KB, 9 KB of which went to keeping the CPU's state through light sleep
(ticket 13). Every boot logs `ThistleOS ready; internal RAM N KB free, largest
block M KB`. To keep it that way:

- **Buffers that only tasks touch go to PSRAM**: `EXT_RAM_BSS_ATTR` on a big
  zeroed static (the App Store, Assistant and Terminal state), or
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. A plain `malloc` under 4 KB lands
  in internal RAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`). Not for DMA buffers,
  anything an interrupt or code running while flash is written reads, or
  secrets: the Vault's entries and key, and `msg_crypto`'s state, stay
  internal. `MALLOC_CAP_SPIRAM` is `1 << 10`; `1 << 9` is a capability no heap
  has.
- **Stacks stay in internal RAM.** A PSRAM stack cannot be read while flash is
  written, so its thread could never read or write SPIFFS, NVS (which the WiFi
  driver writes) or OTA, which is too easy to break.
- **Do not start a thread for a loop that mostly waits.** Make it a job on the
  kernel worker (`thistle/kworker.h`, `kworker.rs`): one 16 KB stack that runs
  jobs one at a time, so it fits the deepest job rather than all of them.
  MeshCore's loop and the GPS reader run there. A job must return promptly,
  since nothing else runs meanwhile; a LoRa send, a second or two, is about the
  longest. Work that waits on a network or a modem has a thread of its own, as
  `net_coordinator`'s does, and UI work polls from an `lv_timer`, as the
  Terminal's UART reader does.
- A Rust `std::thread` shows as `pthread` in task lists unless the spawning
  thread calls `thistle_pthread_name_next()` (`link_deps.c`) first.

### Sleep, deep sleep and the power key

The power manager sleeps the device after an idle time and deep-sleeps it
after a longer one, both set in Settings → Power (`thistle_os.power` in
system.json). Decisions are in `power_policy.rs`, tested on the host; the
running of them in `power_manager.rs`; every ESP-IDF sleep call in
`components/kernel/src/power_platform.c`; the sleep screen and power menu in
`components/ui/src/power_ui.c`. Ticket 13 has the hardware and the reasons.

- **A board sleeps only if its profile's `power` section names a wake pin.**
  On the T-Deck Max: the power key is the BOOT button, **IO0**; the keyboard's
  INT is IO15; the modem's RI is IO7; LoRa DIO1 (IO5) wakes light sleep only.
- **Light sleep takes the USB-Serial-JTAG down.** A sleeping board is not on
  USB: `idf.py flash` and captures fail until a key wakes it. "Stay awake when
  plugged in" (default on) stops the idle timers on USB power, but the power
  key and Settings' "Sleep now" still sleep a plugged-in board. Deep sleep
  takes it off USB too, and waking from it is a restart.
- **Plugged in is the charger's answer, not its charge state.** A full
  battery whose charging has paused reads "not charging" with the cable in;
  the SY6970 answers `is_plugged_in` (the last field of
  `hal_power_driver_t`) from its power-good and bus status. A driver that
  leaves it NULL, as the TP4065B's does, gets charging or charged taken for
  plugged in. The power log says `plugged in` or `on battery` as it changes.
- **Anything sleep would break holds the device awake**:
  `power_manager_hold()` / `power_manager_release()` (`thistle/power_manager.h`,
  `thistle_power.h` for drivers), or `HoldGuard` in Rust. An update, audio,
  a download, an assistant request, a GPS track and a serial session do.
- **A task that draws or reads input** calls `power_ui_join()` once and
  `power_ui_checkpoint()` at the top of each pass, so sleep never lands in
  the middle of a bus transfer.
- **The log's timestamps stop during light sleep**; they count FreeRTOS ticks.

### SMS and the message history

The kernel, not an app, receives SMS: the network coordinator fetches them
from the modem (`sms_manager.rs`, `thistle/sms.h`), keeping it registered
while `thistle_os.cellular.sms` is on even with mobile data off. During a
data session an SMS is noticed by the modem's RI line and fetched by
interrupting the session (ADR-0004). Every SMS and MeshCore message, in or
out, is appended to `/sdcard/messages/{sms,mesh}/<conversation>.jsonl` by
`msg_log.rs`, unless `thistle_os.messenger.save_history` is off (the
Messenger's settings). The simulator's `--sms "SENDER: TEXT"` puts a message
in its fake modem. Ticket 14 has what the hardware has yet to confirm.

Each message received also buzzes the vibration motor and pops up
(`msg_alert.rs`, `thistle/msg_alert.h`), unless
`thistle_os.messenger.alerts` is off or the Messenger has its conversation on
screen (`rs_msg_alert_set_viewing`). The buzz is played where the message is
received, so a LoRa wake buzzes without the UI; the status bar takes the
pop-up. The status bar's envelope counts unread SMS and mesh messages. The
simulator's motor prints `[sim_haptic] message buzz`. Ticket 15.

### Calls and the Phone app

The T-Deck Max makes and takes voice calls through the A7682E, which has a
microphone of its own; its one audio output reaches the speaker through a
switch (`AUDIO_SEL`, XL9555 P12, HIGH = modem). ADR-0008 has the reasons,
`.scratch/phone/` the spec and what the hardware has to confirm.

- **The kernel follows a call by polling `AT+CLCC`**, never by reading URCs.
  A kworker job reads RI once a second (`call_ringing`, a pin read: RI stays
  low while a call rings); a call thread (`call_manager.rs`, 8 KB stack)
  exists only while a call is ringing, being placed or connected.
- **A call suspends a data session**, as an SMS does: the driver holds the
  line from the first call command until `call_list` finds no call, and the
  network coordinator dials, fetches and powers down nothing meanwhile.
- **The audio driver owns the speaker.** `hal_audio_driver_t.set_route` hands
  it to the modem; the ES8311 needs `voice_route_*` in its board entry, and a
  board whose stored `board.json` lacks them makes calls nobody hears.
- **Calls need the modem on**: Settings → Mobile data → "Calls and SMS"
  (`thistle_os.cellular.sms`), on unless switched off.
- **The phonebook is the kernel's** (`phonebook.rs`, `thistle/phonebook.h`):
  every `contacts.vcf` on the card, any case, up to 4 folders deep, read on a
  one-shot thread the first time anything asks. Stars are kept by name in
  `/sdcard/phone/starred.json`; the call log is `/sdcard/phone/calls.jsonl`.
  The Messenger's older `contact_manager.rs` is separate.
- **The call screen is system UI** (`components/ui/src/call_ui.c`), like the
  quick panel: LVGL window managers only. The Phone app (`apps/phone/`) only
  dials; "Message" hands over through `rs_sms_request_compose()`.
- The simulator's fake modem: `--call NUMBER` rings, `--call-hangup S`,
  `--call-preview STATE`, and `--tap dial:N` / `compose:N`;
  `simulator/tests/test_calls.py` and `test_phone_app.py`.

### The clock

The kernel keeps the system clock in UTC and sets it from the best source it
has: NTP over whatever network is up, then GPS while something has it on
with a fix, then the mobile network's time (NITZ, read from the A7682E with
`AT+CCLK?`), then a clock chip. A worse source corrects the clock only after
an hour without a better one, and only beyond its own precision; up to 10 s
is slewed, more is stepped. Decisions are in `time_policy.rs`, the running of
them in `time_manager.rs` (`thistle/time_manager.h`), every ESP-IDF call in
`components/kernel/src/time_platform.c`, which replaces ESP-IDF's weak
`sntp_sync_time()` so NTP is judged like the rest (ADR-0007). Ticket 17.

- **The T-Deck Max has no clock chip.** Its time survives sleep, deep sleep
  and restarts, not a power-on or the reset button (EN resets the RTC
  domain too); until a source answers, the status bar shows `--:--`.
  Anything before 2026 counts as not set (`time_policy::FLOOR_S`).
  Kept, it drifts: the RTC runs on the S3's internal RC oscillator, and a
  20 s deep sleep left it 125 to 142 ms ahead, a reflash 3.9 s out. NTP
  slews it back once a network is up.
- **The zone is a POSIX rule in `TZ`**, so `localtime()` in C apps agrees with
  the status bar. Settings → Date & time picks an IANA name from
  `time_zones.rs`, or Automatic, the mobile network's offset (UTC without
  one). A country that changes its rules needs its line there changed.
  `thistle_os.time` holds `automatic`, `zone`, `format_24h`, `ntp_server`.
- **Show the time with `time_manager_format_time()`**, "14:05" or "2:05 PM";
  give it 12 bytes.
- The boot log's clock line, `2026-10-13 18:29:27 UTC, from NTP; zone
  Automatic (UTC+02:00), UTC+02:00 now`, says what set the clock and the
  zone's offset as newlib reads its rule; each later correction logs its
  source and size (`clock slewing by -125 ms, from NTP`). Like every Rust
  module's on the device, these lines carry no tag and no newline.

### The quick panel

A drag down from the status bar, or the quick key (`HAL_KEY_QUICK`, the
T-Deck Max's middle touch key, `quick_key` in its profile), drops a panel of
switches and sliders over the app in front (`components/ui/src/quick_panel.c`,
`ui/quick_panel.h`); Back, Esc, Home, a tap below it or a drag up close it.
The drag is picked out of the raw touch stream by `pull_gesture.c`, which has
host tests. LVGL window managers only; thistle-tk has none.

- **A switch is a registered struct of callbacks**, `quick_toggle_t` or
  `quick_level_t`; the built-in ones are in `quick_panel_tiles.c`. Add one
  with `quick_panel_add_toggle()` rather than by editing the panel.
- **Its state belongs to the kernel, not the panel**, so Settings and apps
  agree with it: `wifi_manager`, `net_cellular_*`, `gps_manager.rs`
  (`thistle_os.gps.enabled`), `rs_mesh_set_enabled` (which stops MeshCore
  and sleeps the radio), `audio_volume.rs` (`thistle_os.audio`), and
  `light_prefs.rs`, which now keeps the lights' levels. Set the lights
  through `light_set_display()` / `light_set_keys()`, not the drivers, or
  the panel shows a stale level.
- `simulator/tests/test_quick_panel.py`; the simulator's `--drag` and
  `--tap quick` drive it.

### Home

The launcher's Home (`components/apps_builtin/launcher/`) is, top to
bottom, the clock and the date, room for what is new, six pinned apps (four
on a landscape screen) and All apps. `.scratch/home/spec.md` has the rest.
LVGL window managers only; thistle-tk's launcher is its own.

- **The bands are `home_core.c`'s**, host-tested in
  `simulator/tests/test_home_core.c`: where each goes on a screen of a given
  size, which apps are pinned, the date's text. A band the screen has no
  room for is left out.
- **What is new reads kernel state only**, as the status bar does: no app
  has a hook in Home or code for it. Never `rs_msg_alert_take()` (it takes
  the status bar's pop-up) nor `rs_call_missed_seen()` (the Phone app's).
- **Home's widgets live only while it is in front**: built in
  `launcher_ui_show()`, deleted in `launcher_ui_hide()`, so an app has
  Home's share of LVGL's pool. A pinned app launches from an
  `lv_async_call()`, never inside its own tile's click.
- The status bar leaves its clock out while Home shows its own
  (`statusbar_set_clock_visible()`). The clock font is
  `launcher/fonts/home_clock_44{,_mono}.c`, made by
  `launcher/tools/gen_clock_font.sh`.
- `test_launcher_visuals.py` masks Home's clock band as it does the status
  bar.

### Dictation

Holding the microphone key (`HAL_KEY_MIC`, left of Space on the T-Deck Max)
records while a text field has the keyboard's focus; letting go sends the
recording to Cohere and inserts the transcript at that field's cursor
(`components/ui/src/dictation.c`, `ui/dictation.h`). It is not tied to any
app, Hermes or the tailnet. `.scratch/dictation/spec.md` has the rules.

- **The same provider as the Hermes agent**: `POST
  https://api.cohere.com/v2/audio/transcriptions`, model
  `cohere-transcribe-arabic-07-2026`, a 16 kHz mono WAV. The key is
  `COHERE_API_TOKEN` in the repository's `.env`, read into the build
  directory as the Podcasts key is
  (`components/ui/dictation_credentials.cmake`); a card's
  `config/dictation.json` overrides `api_key`, `model`, `url`.
- **The user picks the language**: Cohere detects none, and told `ar` it
  answers some English in Arabic, translated. Arabic unless the quick
  panel's Dictation tile (EN) is on; `thistle_os.dictation.language` in
  system.json, `dictation_prefs.rs`. Arabic is drawn as below.
- **No focused text field, no recording**; password fields, calls and no
  network are refused with a toast, and under 300 ms of audio is a tap.
  60 s at most.
- **What needs no hardware is in `dictation_core.c`**, host-tested in
  `simulator/tests/test_dictation_core.c`: the WAV, the multipart form, the
  answer's JSON, how the transcript joins the text before the cursor.
- **The whole recording is in PSRAM** (1.9 MB for 60 s, freed after), and
  each dictation has a FreeRTOS task with an 8 KB internal stack, gone when
  the transcript is in. The device is held awake meanwhile.
- **A player pauses while anything records**: `rs_audio_capture_begin()` /
  `_end()` / `rs_audio_capturing()` (`thistle/audio_volume.h`). The
  microphone hears the speaker, and the codec runs both directions at one
  rate, so a player sets its own format again after. The Podcasts player
  plays on once the recording ends.
- The codec drops its first 400 ms after opening, so the pill says
  Listening only once audio arrives.
- `simulator/tests/test_dictation.py` runs it against a fake Cohere;
  `THISTLE_SIM_MIC=FILE.wav` is what the simulator's microphone hears, and
  `--tap mic:MS` holds the key.

### Arabic text

Every LVGL label and text field draws Arabic joined and right to left,
whatever font it was given; no app does anything for it
(`components/ui/src/arabic_text.c`, `.scratch/dictation/issues/05-*`).

- **It works beneath the widgets, by `-Wl,--wrap`**: `arabic_text.c` stands
  in for `lv_font_get_glyph_dsc`/`_width` (a glyph in the Arabic blocks that
  the font lacks comes from DejaVu Sans' Arabic at the nearest of 14, 18 and
  22 px, one bit a pixel on e-paper), `lv_draw_label` and `lv_text_get_size`
  (text shaped as it is drawn and measured), and `lv_label_get_letter_pos`,
  `_get_letter_on`, `_is_char_under_pos` (a text area's cursor found in the
  shaped text). The list is in `components/ui/CMakeLists.txt` and the
  simulator's; change both. A wrap catches calls from another file only.
- **What a widget holds is never shaped**, so apps read back, save and send
  the letters, not their presentation forms. The shaper is
  `components/ui/src/arabic_shape.c`, a port of LVGL's `lv_text_ap.c`;
  LVGL's own (`LV_USE_ARABIC_PERSIAN_CHARS`) stays off, as it allocates 8
  bytes a character from the 64 KB pool unchecked.
- **Each text finds its own direction**: `base_dir` is auto on the screen and
  layers, inherited by everything, so Arabic alone is right-aligned with its
  cursor at the left. LVGL's default is left to right.
- The fonts are `components/ui/src/fonts/`, made by
  `components/ui/tools/gen_arabic_fonts.sh`. The Reader, Phone and Podcasts
  shape their own text and use the Reader's 16 px font; text already shaped
  is drawn as it is.
- `simulator/tests/test_arabic_shape.c`, and `test_dictation.py`'s
  `arabic_is_drawn_as_arabic`, which reads a screenshot; the simulator's
  `--type` runs print `Focused cursor: X,Y`. A character in the Arabic blocks
  that no font has is logged once, `no glyph for U+XXXX`.

### Podcasts

`apps/podcasts/` browses PodcastIndex (search, trending, categories), streams
MP3 episodes, and saves them to the SD card to play with no network; its
spec is `.scratch/podcasts/spec.md`.

- **The key and secret are never in the source tree.** CMake reads
  `PODCASTINDEX_API_KEY` and `PODCASTINDEX_SECRET` from the repository's
  `.env` (git-ignored) into `pod_cred_default.h` in the *build* directory
  (`apps/podcasts/credentials.cmake`); a card's `config/podcasts.json`, which
  the app's setup page writes, takes their place. The simulator's SD card is
  in the repository, so its `config/podcasts.json` is git-ignored too.
  Requests are signed with the date, so they fail until the clock is set.
- **The player outlives the app.** Leaving an app destroys it, so the player
  (`pod_player.c`) and downloads (`pod_download.c`) are module statics with
  threads of their own, not the pages'; the quick panel gets a Podcast
  switch while one is loaded. minimp3's 16 KB scratch is in PSRAM
  (`pod_mp3.c`), so the decoder's stack is small.
- **ESP-IDF's open/read HTTP API does not follow redirects**, which podcast
  hosts' links are made of; `pod_net.c` follows them itself. The simulator's
  shim streams for real (curl's multi interface) and does the same.
- `simulator/tests/test_podcasts.py` runs the app against a fake PodcastIndex
  and podcast host; `apps/podcasts/tests/run_tests.sh` has the host tests.

### File Transfer

`apps/filexfer/` shares the SD card and SPIFFS with a browser on the same
WiFi, at `http://thistledev.local` (mDNS, `espressif/mdns`) or the device's
address: list, upload, download, rename, delete, and edit text in the page,
which checks JSON before saving it. `.scratch/file-transfer/spec.md` has the
reasons, and what the hardware has yet to confirm.

- **The server runs while the app is open.** Opening it starts the server
  with a new password, shown on the device; leaving the app, Stop, or 15
  minutes without a request stops it. Meanwhile it holds the device awake.
- **Only on WiFi.** With mobile data on, the server listens on the modem's
  link too; `on_open` in `xfer_server.c` closes any connection that did not
  come in on the WiFi address.
- **Every path goes through `xfer_path_parse()`**, which keeps a request
  inside `/sdcard` and `/spiffs`. SPIFFS has no folders and holds 31 bytes
  of name, the leading `/` included (`xfer_fs.h`).
- **The page is `apps/filexfer/web/index.html`**, embedded by `web.cmake`;
  it fetches nothing from elsewhere. A change needs the session cookie and
  `X-Thistle-Request`, or the password as Basic authentication, which a
  script can use: `curl -u :ABCD-EFGH -T a.txt
  'http://thistledev.local/api/file?path=/sdcard/a.txt'`.
- The simulator's server listens on 127.0.0.1, port `THISTLE_SIM_HTTPD_PORT`
  or 8080; its SPIFFS is the folder `THISTLE_SIM_SPIFFS`, or
  `/tmp/thistle_spiffs`; and it prints the password, as a device never does.
  `simulator/tests/test_file_transfer.py`, `apps/filexfer/tests/run_tests.sh`.

### Tailscale

While the user has it on, the device joins their tailnet over WiFi or
mobile data; any socket then reaches a peer's 100.x.y.z address through
the tunnel, and `getaddrinfo("homelab")` resolves a peer by name (lwIP's
`LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM`, in `tailnet_platform.c`).
Internet traffic does not go through it. `apps/tailscale/` shows and
switches it; the quick panel has a tile; the Terminal has `tailscale`
(status, peers, up, down, `ping NAME [PORT]`, a timed TCP connect).
`.scratch/tailscale/spec.md` has the reasons, ADR-0009 the choice of
client.

- **The client is MicroLink**, from `MedAouadhi/microlink` branch
  `thistle`, pinned by commit in `components/kernel/idf_component.yml`, S3
  only. The fork is the one hermes_esp32 pins plus eleven commits; read them
  before moving the pin, and delete `dependencies.lock` when you do (which
  also lets other components move: put back what you did not mean to).
  Decisions are in `tailnet_policy.rs`, the running of them in
  `tailnet_manager.rs` (`thistle/tailnet.h`), every MicroLink call in
  `components/kernel/src/tailnet_platform.c`.
- **Pairing compiles nothing in.** A device that has not joined signs in
  at a URL the control plane gives, shown as a QR code, and joins as the
  signing-in user's device. After that it rejoins with its own identity,
  MicroLink's NVS namespaces `microlink` and `ml_peers`; `tailnet` holds
  whether it has joined. Sign out erases them; the device stays in the
  admin console until removed there. NVS is not encrypted. There is no
  auth-key path, by choice for now; MicroLink still takes a key.
- **A packet that finds no WireGuard session starts one** (MicroLink's WG
  manager, through DERP and DISCO's direct path) and is dropped: a first
  connect to a peer takes a TCP retransmit, about a second.
- **Data goes through DERP until a direct path is proven**, by a DISCO pong
  or a packet that came straight from the peer; over mobile data, behind
  the carrier's NAT, it always does. The WireGuard netif's link stays up
  with no session: wireguard-lwip took it down, and lwIP then sent tailnet
  packets out of the modem or WiFi, unencrypted, where nothing started a
  session: over mobile data no peer could be reached unless it had
  reached the device first.
- **A session lapses once idle**, as Tailscale's do: no persistent
  keepalive, and a handshake is one attempt, not a peer kept connected for
  good. Before, every peer that had had a session kept one, with a
  keepalive every 25 s.
- **An idle tailnet is quiet**, as Tailscale's own client is. DISCO looks
  for a direct path only to a peer whose session carried a packet in the
  last 45 s; STUN goes every 23 s while one does and every 5 minutes
  otherwise; an endpoint update goes only with something new in it, and
  a STUN port alone is new only while a session is in use (idle, a mobile
  network's NAT hands each probe a new one). Nothing is sent to keep the
  control connection or DERP: their servers send keep-alives about once
  a minute each, and either is reconnected after 120 s without a word.
  Idle on LTE it sends 1.4 packets a minute, where it sent 158; the two
  servers' keep-alives still wake the modem about twice a minute.
  `.scratch/tailscale/issues/01-*` has the counts and how to take them.
- **It needs a network and the clock**: a WireGuard handshake carries the
  time. It is restarted whenever the underlay changes, WiFi to LTE, a new
  address, or a sleep, and stopped while there is none.
- **It costs about 31 KB of internal RAM while on**, MicroLink's four task
  stacks most of it, and about 2 KB off. The log says
  `connected as thistle-a1b2c3, 100.101.102.103; internal RAM 110 KB free`.
  MicroLink's chattiest tags are turned down to WARN (`ml_derp` to ERROR)
  in `tailnet_platform_start()`; raise them there to debug it.
- **Start and stop run on a FreeRTOS task, not a `std::thread`.** On
  ESP-IDF a Rust thread leaves its `Thread` handle behind when it ends,
  about 150 bytes of internal RAM, which a heap trace caught at two
  threads a restart; the task leaves nothing. (`call_manager.rs` still
  spawns one per call.)
- **The kernel worker polls the client once a second**, in one try of the
  platform's lock (`tailnet_platform_poll`), and keeps what it read for
  the status, the peers and the Terminal, so none of them touch the
  client. A client whose stop timed out is kept, not freed, and stopped
  again before anything starts.
- **The home DERP region is measured**, by timing a TCP connect to each
  region, and kept in NVS (`derp_home`): MicroLink's default is Dallas.
- The simulator's client is pretend (`THISTLE_SIM_TAILNET` = `login`,
  `approval` or `refuse`; `THISTLE_SIM_TAILNET_LOGIN_S`;
  `THISTLE_SIM_TAILNET_PEERS=host` takes the host's own peers from
  `tailscale status --json`); whether it has joined is a file in
  `THISTLE_SIM_SPIFFS`. `simulator/tests/test_tailscale.py`.

### Hermes

`apps/hermes/` is a Hermes Device Bridge client, the user's Hermes agent as
hermes_esp32's device has it: one conversation, Markdown replies, tool
calls as activity rows, Stop and New conversation, the Command panel, the
drawer's Hermes Sessions and Scheduled Runs. `.scratch/hermes/spec.md` has
the reasons; hermes_esp32's `protocol/README.md` and its plugin's
`bridge/service.py` are the authority on the protocol (version 1).

- **Over the tailnet only.** The app says what Tailscale is waiting for
  until the device is on the tailnet, then opens a WebSocket to the bridge,
  `ws://100.125.204.43:8765` on homelab. The address is `.env`'s
  `HERMES_BRIDGE_URL` (`apps/hermes/bridge_url.cmake`) or the card's
  `config/hermes.json`, which the app's Address page writes, listing the
  tailnet's peers.
- **The bridge admits a node by its tag, `tag:hermes-device`**, and reads
  no credential. This device joins as its user's, untagged, so the bridge
  answers `AUTHENTICATION_FAILED` until the tag is given in the Tailscale
  admin console, and the tailnet's policy lets the tag reach the port; the
  app says so and tries every 30 s. A first admitted connection is the
  device's registration.
- **Typed or dictated, sent as text.** The compose field is an ordinary
  text field, so the mic key dictates into it (Dictation, above); HELLO
  does not ask for `spoken_message`.
- **The session lives while the app is open**, on a FreeRTOS task with an
  8 KB internal stack (`hermes_bridge.c`); the chat state, a 64 KB receive
  buffer and the outbox are one PSRAM allocation. The screens read a copy
  and queue what they send; nothing is queued while the link is down.
  Lost, it is opened again a second later, then up to 30 s apart. The
  device is held awake while a turn is in flight, ten minutes at most.
- **The WebSocket is the app's own** (`hermes_ws.c`, `hermes_ws_conn.c`):
  `ws://` only, BSD sockets, so the simulator runs the same code. The
  handshake's SHA-1 and the JSON reader are the Podcasts app's
  (`pod_sha1.c`, `pod_json.c`).
- **Markdown is md4c 0.5.2 with hermes_esp32's `hermes_markdown.c`**; a
  table wider than the screen becomes `Header: value` groups. Labels point
  at text kept in a PSRAM arena per item (`lv_label_set_text_static`), not
  LVGL's pool. `hermes_glyphs.c` writes what Montserrat lacks as ASCII or
  LVGL symbols (quotes, dashes, accented letters, a few emoji).
- `apps/hermes/tests/run_tests.sh` has the host tests;
  `simulator/tests/test_hermes.py` runs the app against
  `fake_hermes_bridge.py`, a stdlib bridge that speaks as the plugin does.

### LVGL's pool is 64 KB

LVGL allocates every object and every local style property from its own
64 KB pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`), not the system heap. A page
styled one property at a time costs over 1 KB a row, and Settings' zone
picker ran the pool dry with a dozen rows on top of the page beneath it.
LVGL's malloc assert then spins forever inside the layout code, which looks
like a hang, not an out-of-memory error. Give a long list shared
`lv_style_t`s, close the page beneath it, and measure with
`lv_mem_monitor()`; the simulator's pool is the same size. The launcher
deletes its app drawer before opening an app from it: its nineteen tiles,
styled a property at a time, once left Files too little for a card of
nine folders. Home's widgets go the same way whenever an app comes to the
front; in the simulator a pool run dry shows as a segfault in
`lv_draw_add_task`, not a hang.

### Rust toolchains

Two toolchains, for two different jobs:

- **`esp`** — the Xtensa fork, for anything targeting the device (`cargo +esp check`, `cargo +esp build`). Installed by `espup`, *not* by rustup.
- **host stable** — for the kernel test suite, which runs natively. Pass your own host triple to `--target`; the crate's `.cargo/config.toml` defaults to `xtensa-esp32s3-espidf`, so an unqualified `cargo test` cross-compiles and dies with `'esp32s3' is not a recognized processor for this target`.

Install or update the `esp` toolchain with:

```sh
cargo install espup --version 0.16.0 --locked --force
espup install
```

**Pin espup to 0.16.0.** 0.17.x aborts with `Unsuported file extension: 'part'`, and CI pins the same version (`.github/workflows/build.yml`) — keep the two in step.

The `esp` toolchain must be recent enough for the dependency tree: `cargo +esp check` fails at manifest-parse time with `feature 'edition2024' is required` on anything older than Xtensa Rust 1.85. espup 0.16.0 currently installs 1.97.0.0. If you see that error, your `esp` toolchain is stale — reinstall with the commands above rather than editing dependency versions.

Do **not** source `~/export-esp.sh` in the shell you run `idf.py` from — it overrides ESP-IDF's GCC with an incompatible version and breaks the C build. `components/kernel_rs/CMakeLists.txt` writes a throwaway script that sources it in an isolated subshell for the cargo invocation alone, which is exactly why the firmware build works without it in your environment.

## Architecture (three-tier immutable trust chain)

```
Recovery (Rust, ota_0, 1MB) — immutable, root of trust
  ↓ Ed25519 verify
Kernel (100% Rust, ota_1, 4.5MB) — immutable, hardware-independent
  ├── Display Server (surfaces, compositor, input routing)
  ├── Kernel modules (57 modules: app manager, IPC, events, permissions, signing, manifest, crypto, HAL registry, drivers, WM, app store, ...)
  ├── Syscall table (~80 syscalls exported to loaded ELFs)
  └── HAL registry (bus handles, driver vtables)
         ↓ loads from
SPIFFS (10.5MB) + SD card — updateable
  ├── config/boards/ (per-board board.json: hardware pins, buses, driver list)
  ├── config/system.json (user prefs, WM selection, WiFi)
  ├── drivers/*.drv.elf (hardware drivers)
  ├── apps/*.app.elf (all apps)
  ├── wm/*.wm.elf (window managers)
  └── themes/*.json (UI themes)
```

### Hardware independence principle

The kernel is **100% Rust and hardware-independent**. It never calls ESP-IDF hardware APIs directly. All hardware interaction goes through HAL driver vtables:

- **Display, Input, Radio, GPS, Audio, Power, IMU, Storage, Network, Crypto, RTC, Haptic** — all accessed exclusively via their respective `hal_*_driver_t` vtables.
- **Crypto** — the `hal_crypto_driver_t` vtable allows hardware acceleration (e.g. ESP32-S3 hardware AES/SHA engines) when a crypto driver is registered. If no hardware crypto driver is present, the kernel's built-in Rust software implementation is used as a transparent fallback.
- **RTC** — the `hal_rtc_driver_t` vtable provides timekeeping (e.g. PCF8563). Falls back to system tick when absent.
- The kernel itself does not know or care which physical chip is present — that detail lives entirely in the loaded `.drv.elf` files and `board.json`. Component-level hardware detection (I2C/SPI/UART scanning) is handled by the Recovery OS.

### Key components
- **HAL** (`components/thistle_hal/`): Pure vtable interfaces. Bus handle sharing (SPI/I2C). 12 HAL interfaces: display, input, radio, GPS, audio, power, IMU, storage, network, crypto, RTC, haptic. Haptic was appended to the end of `hal_registry_t` so no existing field moved (ADR-0005); a new interface goes there too.
- **Drivers** (`components/drv_*/`): 16 drivers (13 Rust + 3 C, compiled-in + standalone `.drv.elf`): e-paper, LCD, OLED, keyboard, touch (×2), GPS, accelerometer, power, audio, RTC (PCF8563), SD card, IMU (QMI8658C), light sensor. C drivers for modem (esp_modem), radio (RadioLib) and the ES8311 audio codec (esp_codec_dev).
- **Networking** (`components/kernel_rs/src/net_manager.rs`, `net_coordinator.rs`): WiFi and LTE (the A7682E driver) register as `hal_net_driver_t` transports. esp_netif routes over WiFi whenever it is up and LTE otherwise, with DNS following the route (ADR-0003). The coordinator keeps mobile data up while Settings has it on, and rejoins the saved WiFi network after a drop unless WiFi is switched off in Settings (`wifi.radio_off`, which stops the radio).
- **Board config**: Per-board `board.json` files in `sdcard_layout/config/boards/` define pins, buses, and which drivers to load. Replaces compiled `board_*` components. 10 boards supported.
- **Kernel (Rust)** (`components/kernel_rs/`): 100% Rust. 57 modules: manifest parser, permissions, IPC, events, app manager, version, crypto, HAL registry, driver manager, WM, app store, wifi/ble/net managers, OTA, and more. 1231 tests.
- **Display Server** (`components/kernel/src/display_server.c`): Surface management, compositor, WM vtable interface.
- **UI** (`components/ui/`): thistle-tk WM (default). LVGL-based WM also available. Both loadable as `.wm.elf`.
- **Apps**: 19 apps, migrating to `.app.elf` on SPIFFS. Five remain in
  `components/apps_builtin/`; the other thirteen are their own components under
  `apps/`, listed once in `cmake/thistle_apps.cmake` for the simulators and
  found by `EXTRA_COMPONENT_DIRS` for the firmware.
- **App Store** (`components/kernel_rs/src/tk_appstore.rs`): Browsable Rust app. Rich catalog with ratings, categories, download counts, changelogs, and component-level driver matching for arch-aware installs.
- **Recovery** (`recovery/`): Rust. WiFi AP + captive portal, OTA flashing, I2C/SPI/UART hardware scanning, multi-arch chip detection, 3-step web UI provisioning flow.
- **Simulator** (`simulator/`): SDL2, real kernel code, fake WiFi/BLE, libcurl HTTP.

## Key Conventions
- License: BSD 3-Clause. No GPL dependencies.
- HAL interfaces: C structs of function pointers (vtables).
- Apps/drivers communicate via syscall table (C ABI).
- Drivers get bus handles via `hal_bus_get_spi()`/`hal_bus_get_i2c()`.
- Manifests: `manifest.json` alongside every `.app.elf` / `.drv.elf`.
- Signing: Ed25519 (Monocypher). Signed = full permissions. Unsigned = IPC only.
- Rust kernel modules expose `rs_*` C FFI functions matching the C API.
- All public headers use `#pragma once`.
- Error handling: `esp_err_t` return codes (C), i32 ESP error codes (Rust FFI).
- Logging: `ESP_LOG*` macros (C). Rust logging TBD.
- C kernel shims: ~210 LOC remaining — `kernel_shims.c` (87 LOC weak link stubs) and `tk_wm_shims.c` (123 LOC HAL bridges).

## Adding a New Driver
**As standalone .drv.elf** (preferred):
1. Use `driver_sdk/` — include `thistle_driver.h`
2. Implement `driver_init(const char *config_json)`
3. Get bus handles: `hal_bus_get_i2c(0)`, `hal_bus_get_spi(0)`
4. Register vtable: `hal_*_register(&my_driver, NULL)`
5. Build with `thistle_driver()` CMake function or Rust cdylib
6. Add entry to `board.json` with pin config

**As compiled-in** (fallback):
1. Create `components/drv_<name>/`
2. Implement HAL vtable
3. Add to board definition

A driver that drives its own SPI chip select holds the bus while it is low
(ADR-0006).

## Adding a New Board
1. Create `sdcard_layout/config/boards/<board_name>.json` with pin assignments, bus configs, driver list
2. Place on SD card (or SPIFFS)
3. Kernel reads it at boot, initializes buses, loads drivers
4. No recompilation needed
5. Recovery OS can auto-detect attached hardware via I2C/SPI/UART scanning during provisioning

## Adding a New Window Manager
1. Implement `display_server_wm_t` vtable (init, render, on_input, etc.)
2. Build as `.wm.elf`
3. Set `"window_manager"` in `system.json`
4. Or: register compiled-in via `display_server_register_wm()`

### Choosing among the compiled-in managers

`"window_manager"` in `system.json` takes a manager's own `name`:

- `"lvgl-epaper-wm"` — LVGL with deferred refresh. The default on e-paper.
- `"lvgl-lcd-wm"` — LVGL with direct flush. The default on LCD.
- `"thistle-tk"` — the Rust WM. Works on e-paper and LCD, with cheaper
  e-paper refreshes, but hosts only its launcher, App Store and Flashlight.
- `"auto"`, empty or absent — pick from the display type, as above.

`thistle_wm_select()` (`components/ui/src/wm_select.c`) resolves this for the
firmware and both simulators; a name it does not recognise logs a warning and
falls back rather than failing the boot.

The choice decides more than how things are drawn. The LVGL built-in apps
parent their widgets to `ui_manager_get_app_area()`, which is NULL unless an
LVGL manager ran `ui_manager_init()`, so they are registered only under one —
launching them under thistle-tk segfaults. App Store and Flashlight exist in
both toolkits under the same IDs, and the app manager keeps the first
registration of an ID, so each manager registers only its own:
`lvgl_apps_register_builtin()` under LVGL, `tk_native_apps_register()` under
thistle-tk. All nineteen apps are reachable by default on every S3 board;
naming `thistle-tk` trades sixteen of them for its refresh behaviour.

`simulator/tests/test_app_matrix.py` launches every app on every simulated
device profile, and the simulator's `--wm NAME` overrides `system.json` for a
single run.

## Agent skills

### Issue tracker

Issues and specs live as markdown files under `.scratch/<feature-slug>/`. See `docs/agents/issue-tracker.md`.

### Domain docs

Single-context: `CONTEXT.md` + `docs/adr/` at the repo root (created lazily as needed). See `docs/agents/domain.md`.
