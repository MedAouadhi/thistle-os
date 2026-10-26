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
- **CI**: GitHub Actions on GitHub's hosted runners, every push to main and every pull request. `ci.yml`: the firmware for each chip in its list (the S3 alone) in `espressif/idf:v5.5`, failing if a `.env` is in the workspace, its partition size check, Recovery, cargo-audit, Semgrep SAST, Trivy. `tests.yml`: the host tests (Rust kernel, Python tools, simulator). `pages.yml`: the docs and the WASM simulator to the repository's own Pages. `tools/test_ci_workflows.py` keeps them off self-hosted runners and upstream's deploy targets.

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

### The board profile follows the firmware version

A board writes its profile to `/spiffs/config/board.json` from the embedded
copy in `components/kernel/src/board_fingerprint.c`; the firmware build makes
no SPIFFS image. The stored profile says which firmware wrote it,
`"written_by": "0.6.0"`, added when the file is written (the embedded copies
and the catalog files stay equal, as CI checks). At boot, a profile another
version wrote, or one that names no writer, is rewritten from the running
kernel's embedded copy, and the previous one is kept as
`board.json.bak`, the only backup (`board_config.rs`,
`follow_firmware_version`; `update_policy::board_profile` decides). So a USB
flash, an update and going back all bring their own profile, and
`system.json`, WiFi and the mesh identity are untouched. A board id the kernel
embeds no profile for keeps its profile. The boot log says `board profile
tdeck-max rewritten from 0.6.0's built-in copy (it was 0.5.2's)`. A hand edit
to `board.json` lasts until the next version boots; it is then in the backup.
Before this (tickets 03, 06, 09 and 10), a changed profile never reached a
board that had one.

### Two kernel slots

The flash holds two 4.5 MB kernel slots, `kernel_a` (ota_0) and `kernel_b`
(ota_1), and a 2.5 MB `recovery` slot (ota_2) that nothing writes yet
(`partitions.csv`, ADR-0010). NVS, otadata, SPIFFS and the core dump are
where they were before, so the one USB flash that moves a board to this
table keeps WiFi, the mesh identity, `board.json` and `system.json`. There is
no `factory`: `idf.py flash` writes `kernel_a` and resets otadata, so a USB
flash always boots `kernel_a`.

- **An update is written into the other kernel slot, named by its label**
  (`kernel_slot.rs`, `ota.rs`; every ESP-IDF call in
  `components/kernel/src/kernel_slot_platform.c`). Never
  `esp_ota_get_next_update_partition()`: from `kernel_b` it is `recovery`.
  The slot is selected only once the bytes written are the artifact
  manifest's and ESP-IDF has validated the image.
- **A firmware installs only if its artifact manifest holds up**
  (`update_policy.rs`, host-tested; `update_manager.rs` runs it): the
  manifest's signature verifies against the kernel's key (never an app
  publisher's), it is a firmware, for this chip (`manifest::current_arch()`)
  and for the stored board id, at or above the security floor, and the file
  is its size and SHA-256. The manifest's format and shape checks are the
  shared crate `crates/thistle-artifact`, Recovery's module plus prerelease
  versions, destination `kernel`, plain-HTTP URLs and the arch `host`;
  Recovery keeps its own copy. A card install may be any version at or above
  the floor. Settings → System update installs a release copied to
  `/sdcard/update/` (`<name>.bin`, `.bin.manifest`, `.bin.manifest.sig`, as
  the release assets are named), with the reason for a refusal; the log says
  `card update refused: <reason>`. The C API is `thistle/update.h`.
- **The security floor is Recovery's NVS key, `thistle_sec`/`fw_sec_ver`**,
  beside each slot's record (`kernel_a_ver`, `kernel_a_sec`, ...), all in
  `components/kernel/src/update_platform.c`. A slot's record is written with
  the slot and by a kernel for its own slot at boot; the floor rises to a
  kernel's security version (`version::SECURITY_VERSION`) only when that
  kernel is kept, so one rolled back never raises it. The boot log says
  `security floor N; this kernel's security version M`, and `security floor
  raised from N to M` at the keep.
- **The device updates itself from the catalog** (ticket 07). Once a day
  with a network up, at most once a boot while the clock is unset, a
  kernel worker job (`update_manager.rs`, `tick`) starts the check on the
  `update` task: a FreeRTOS task with a 16 KB internal stack, made for one
  job (a check, a download, a card install) and gone after. It reads the
  catalog the App Store reads (`appstore_get_catalog_url()`), keeps it at
  `/sdcard/appstore/catalog.json`, and offers the newest firmware entry
  newer than the running version for this chip and board
  (`update_policy::pick_offer`); the App Store's Refresh counts as a check
  (`catalog_read`). `/spiffs/update.json` records when and what. Download
  and install fetches the manifest and signature, checks them as a card's
  (and newer than what runs), then streams the image through the redirects
  (`appstore_client::HttpStream`) into the other slot. On mobile data it
  waits (`UPDATE_WAITING`) until the user allows it or WiFi comes; the
  log says `the download waits: ...`, `update check (...): ...`,
  `update refused: ...`. Only firmware entries are parsed into JSON
  values: a whole catalog's tree of small allocations would land in
  internal RAM. The App Store has no Firmware tab. The simulator checks
  daily only with `THISTLE_SIM_UPDATE_CHECKS=1` (its card names the real
  catalog); `THISTLE_SIM_NETWORK=mobile` takes its host network down, so
  with mobile data on the fake modem is the network; `THISTLE_SIM_CA_BUNDLE`
  lets a test's server speak HTTPS. `test_system_update.py` runs it against
  a fake GitHub.
- **Settings → System update offers Go back** to the version in the other
  kernel slot, with no download, while that slot's record is the version its
  image description says (`esp_ota_get_partition_description`) and its
  security version is at or above the floor (`update_policy::go_back`,
  `rs_update_go_back*`). An install clears the record before it erases the
  slot, so Go back is gone from then on, whether or not the install finishes.
  Going back selects the slot as it is and restarts; that kernel boots new,
  kept after a minute like an update, and rewrites the board profile from
  its own embedded copy. The simulator models the erase in its otadata.
- **A new kernel is kept after 60 s of running**
  (`ota_keep_new_kernel()`); until then a reset rolls it back to the other
  slot (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). On that first boot the
  power manager starts only once it is kept, as deep sleep is a reset; on
  every other boot nothing waits. The boot log says `running kernel_b,
  version ..., new: kept after 60 s` and then `kept the kernel in kernel_b`.
- **CI measures the kernel against a kernel slot**
  (`tools/check_partition_sizes.py`). ESP-IDF's warning that the kernel is
  larger than the smallest app partition, the reserved one, is expected, as
  is its warning that `recovery` is also the name of a data subtype.
- The simulator's slots are files in `THISTLE_SIM_SLOTS` (or
  `/tmp/thistle_slots`): `kernel_a.bin`, `kernel_b.bin`, and `otadata`, the
  bootloader's choice and the version each slot's image describes, which is
  the version a run reports. A run that ends before the keep leaves the next
  on the other slot. Its NVS is `nvs` there too, and a slot's security
  version the one recorded when it was written. `THISTLE_SIM_KEEP_S`
  shortens the keep, `THISTLE_SIM_PUBLISHER_KEY` names a public key it
  trusts for firmware manifests (simulator builds only, never the host
  tests), its board id is the `--device` name, and `--install-update`
  installs the card's release once booted, exiting where a device restarts.
  `simulator/tests/test_kernel_slots.py`, `test_system_update.py`.

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
Messenger's settings). The Messenger reads them back the first time it opens
after a restart (`rs_msg_log_read_back`): the 12 latest conversations, the
last 20 messages of each. Only what earlier boots saved comes back, as this
boot's messages reach it from the kernel's inboxes; `msg_log.rs` marks where
each file ended before this boot first wrote to it. The simulator's
`--sms "SENDER: TEXT"` puts a message in its fake modem. Ticket 14 has what
the hardware has yet to confirm; `test_messenger_history.py`.

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

### GPS

The T-Deck Max's receiver is a u-blox MIA-M10Q on UART2
(`drv_gps_mia_m10q.rs`), powered by XL9555 P02, which only deep sleep and
power-off take down. Ticket 06 has the bring-up and the bench results.

- **Standby is UBX-RXM-PMREQ's 16-byte form**, the only one its firmware
  (ROM SPG 5.10) defines, with `force`. The receiver goes there while GPS is
  off and while the device sleeps, and keeps what it knows for a quick
  start. The M8's 8-byte form, sent until 2026-10, sometimes left it in a
  standby one byte did not wake: GPS stayed off from the first light sleep.
- **Anything sent to it wakes it**, so a receiver in standby is sent nothing
  but the wake pulse, sixteen zero bytes, which the reader repeats every 3 s
  until it talks. A restart leaves the receiver as it was.
- **What it hears is GSV's**, through `get_sky` (`hal/gps.h`): satellites in
  view, heard, and the best C/N0, shown in Settings → GPS. Until the fix the
  log says `no fix after 30 s: 4 satellites in view, 4 heard, best 27
  dB-Hz`, at doubling gaps. Satellites in use stay at 0 until the fix, so
  they tell a weak signal from no signal at all only with these.
- **AssistNow Predictive Orbits**, u-blox's free service, starts it in
  seconds (`gps_assist.rs`). With a Thingstream device profile's token,
  `ASSISTNOW_ZTP_TOKEN` in `.env` (firmware only, never the simulator) or
  `ztp_token` in the card's `config/assistnow.json`, the kernel registers
  the receiver once (its UBX-SEC-UNIQID and UBX-MON-VER, the driver's
  `get_identity`), fetches 7 days of orbits and the almanacs every 20 hours
  while GPS is on, keeps them in `/spiffs/assistnow.{json,ubx}` with the
  last fix, and hands them, the time and that fix to the driver's `assist`
  at each start. No position is ever sent. Without a token nothing is asked.
- **The receiver takes orbit predictions for GPS and GLONASS only**
  (datasheet table 5), so the driver switches GLONASS on at each wake
  (UBX-CFG-VALSET, RAM and BBR), which moves BeiDou from B1I to B1C: B1I
  cannot run with GLONASS. A change of signals restarts its GNSS, so the
  rest waits a second. It sends today's orbits and the almanacs, 200 bytes
  a pass, with UBX-MGA-ACK asked for, and logs `assistance: time taken,
  position taken, 79 of 79 almanacs and 53 of 53 orbits taken`.
- **AssistNow Autonomous is on** (CFG-ANA-USE_ANA, a VALSET of its own
  after the settle, RAM and BBR): the receiver predicts, for three days,
  the orbits it decodes, Galileo's and BeiDou's too, whose AssistNow
  predictions it does not take. The driver matches each UBX-ACK to its
  VALSET in order (`cfg_awaited`).
- **The time handed over is as good as it is, never better**: the clock
  plus the slew still to come, good to its source's accuracy, 0.5 s, and
  50 ppm of the time awake and 1 % of the time asleep since the last sync
  (`power_manager::slept()` counts the sleep). A much better time, NTP's
  after the boot's hand-over, is handed over again until the fix.
  Thingstream's `allowedData` is one string, not the guide's list. Free
  profiles allow 2 requests a day (300 a month on an evaluation one).
  Ticket 06 has the protocol and the limits;
  `simulator/tests/test_gps_assist.py` runs it against a fake service.
- **Each start is reported on the card**, `gps/starts/NNNN-YYYYMMDD-HHMMSS.jsonl`
  (`gps_report.rs`): what was handed over, every 5 s each satellite the
  receiver knows of (UBX-NAV-SAT through the driver's `get_satellites`:
  signal, used, the orbit it is navigated by and those held), the fix,
  and why it ended: 30 s after the fix, `by a sleep`, after 30 minutes.
  No position; the last 20 kept; `"report": false` in
  config/assistnow.json stops it. Read it to see why a start was slow.
  Nothing holds the device awake while GPS looks for a fix, so a sleep
  starts the search again.

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
bottom, the clock and the date, up to two rows of what is new, six pinned
apps (four on a landscape screen) and All apps. `.scratch/home/spec.md` has
the rest. LVGL window managers only; thistle-tk's launcher is its own.

- **The bands are `home_core.c`'s**, host-tested in
  `simulator/tests/test_home_core.c`: where each goes on a screen of a given
  size, which apps are pinned, the date's text, and which rows of what is
  new show and what they say (`home_now_rows()`). A band the screen has no
  room for is left out.
- **What is new reads kernel state only**, as the status bar does: no app
  has a hook in Home or code for it. The rows are a fixed set: new messages,
  a missed call (its button calls back with `rs_call_dial()`), the tailnet
  waiting for a sign-in, updates (the ThistleOS version on offer and the
  number of app updates, `thistle/update.h`; it opens Settings at System
  update with `settings_app_open_next()`, or the App Store when only apps
  have them), else "All caught up" and the mesh. Never
  `rs_msg_alert_take()` (it takes the status bar's pop-up) nor
  `rs_call_missed_seen()` (the Phone app's), and the phonebook only for a
  missed call's name. Read once a second; a label is rewritten only when its
  text changes, as each rewrite is an e-paper refresh. The launcher logs
  `(launcher_ui) now: ...` when the rows change; `test_home.py`.
- **Home's widgets live only while it is in front**: built in
  `launcher_ui_show()`, deleted in `launcher_ui_hide()`, so an app has
  Home's share of LVGL's pool. A pinned app launches from an
  `lv_async_call()`, never inside its own tile's click.
- The status bar leaves its clock out while Home shows its own
  (`statusbar_set_clock_visible()`). The clock font is
  `launcher/fonts/home_clock_44{,_mono}.c`, made by
  `launcher/tools/gen_clock_font.sh`; the rows' detail line falls back on
  `home_latin_14{,_mono}.c` (`gen_latin_font.sh`) for a name's accents.
- The keyboard's focus starts on All apps, unseen, so the first Tab goes
  round to the first row.
- `test_launcher_visuals.py` masks Home's clock band as it does the status
  bar, and runs on a scratch copy of the card.

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
switches it; the quick panel has a tile; the status bar shows
`LV_SYMBOL_SHUFFLE` while it is connected (LVGL window managers); Home
has a row while it waits for a sign-in; the Terminal has `tailscale`
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

### To-do

`apps/todo/` is a checklist: type a task and press Enter, tap it when it is
done, hold it to remove it; Clear done, in the title bar, removes what is
ticked off. From the keys, Tab or Down reaches the list, Enter or Space
ticks a task off, Backspace removes it, and typing goes back to the field.

- **The list is `/sdcard/todo.md`, a Markdown checklist** (`- [ ] Buy
  milk`, `- [x] Pay the rent`), saved whole at each change by way of
  `todo.md.tmp`, so File Transfer's page can edit it too; a line with no box
  reads as a task not done. The list and the format are `todo_core.c`'s,
  host-tested by `apps/todo/tests/run_tests.sh`.
- **The tasks are one `lv_table`**, as Phone's lists are, the tick box and
  the strikethrough drawn in its `LV_EVENT_DRAW_TASK_ADDED`
  (`table_draw_cb`). A row of three widgets cost 1.2 KB of LVGL's pool and
  ran it dry at about thirty tasks; a table row costs its text. At most 50
  tasks, of 80 characters each.
- **A tap leaves the keyboard in the field**, so tasks are typed one after
  another; the table takes it only from Tab or Down.
- `simulator/tests/test_todo.py`. `--type` fills a 32-event key queue, so a
  run types one short task, and a case that needs a list writes `todo.md`
  first.

### App Store

`apps/appstore/` browses a catalog, installs apps, drivers and window
managers from it, and updates the firmware; `appstore_client.rs` does the
network and the files. LVGL window managers only; thistle-tk has its own
(`tk_appstore.rs`).

- **The catalog is a static JSON file**, by default the fork's
  `https://medaouadhi.github.io/thistle-os/store/catalog.json`, which
  `pages.yml` builds from the repository's GitHub releases on every deploy
  (`tools/build_catalog.py --releases`): the newest firmware by version and
  the newest release of each app, each release's own
  `*.catalog-entry.json` asset, drafts and prereleases left out. It holds
  nothing of its own. A card's `config/appstore.json`,
  `{"catalog_url": "..."}`, names another. Refresh keeps the server's JSON
  whole at `/sdcard/appstore/catalog.json`, which the app reads with the
  kernel's parser when it opens (`appstore_load_catalog_file`). Entries for
  another chip are left out.
- **A catalog and its packages can be on the card**: a `file:///sdcard/...`
  URL is read from it, no network needed (`card_file()`), and nothing off
  the card. `tools/make_card_appstore.py` builds `standalone_apps/` into such
  a folder, `card-appstore/` (git-ignored), to copy onto a card's root.
- **Notes, Weather, WiFi Scanner and Flashlight come from the App Store**,
  not the firmware: their packages are `standalone_apps/`, their LVGL
  versions under `apps/` are built nowhere (`THISTLE_STORE_APPS` in
  `cmake/thistle_apps.cmake`). Tests that need one in front use To-do; the
  Arabic dictation case installs the packaged Notes on its card.
- **An app is released by pushing a tag** `<app>-vX.Y.Z` (`notes-v1.0.1`),
  for those four alone (`.github/workflows/release-app.yml`,
  `tools/release_app.py`). The tag is refused unless its version is the
  app's `manifest.json` version and its `min_os` is no newer than the
  newest firmware release, prereleases included. The release sequence is
  computed, major x 10^6 + minor x 10^3 + patch, and written into the
  package; the notes are `metadata.json`'s changes for that version, which
  must be its newest release. Raise both files' `version` and add the
  release to `metadata.json` before tagging.
- **An app installs only as a signed TAP package** (`docs/app-package-format.md`):
  its entry needs `package_url`, `package_sig_url`, `package_sha256`,
  `package_size_bytes` and `release_sequence`. It goes to
  `/sdcard/apps/<id>/generations/<N>/`, `<id>/active.json` naming the
  release. The boot scan (`app_elf_paths`) loads that and each plain
  `/sdcard/apps/*.app.elf`, so an install opens after a restart; the
  Installed tab lists the same (`installed_apps.rs`).
- **App updates are the kernel's to decide** (`update_policy::app_offers`):
  a catalog release of an installed package with a higher release sequence
  and a `min_os` the running firmware meets. Every catalog read counts them
  (the daily check, Check now, Refresh); `rs_update_app_offer()` tells the
  Installed tab, whose details then have Update. The catalog lists no app
  release that needs newer firmware, and the TAP installer refuses a
  package whose own `min_os` is not met (`ESP_ERR_INVALID_VERSION`). The
  simulator trusts `THISTLE_SIM_PUBLISHER_KEY` for apps as for firmware.
- **Signatures are checked against the key in `kernel_boot.rs`**: upstream's
  production key in a release build, the dev key in a debug one. Apps, and
  only apps, may also be signed by the build machine's publisher key:
  `make_card_appstore.py` makes `tools/local_publisher_{private,public}.key`
  (both git-ignored) on its first run, and a firmware built afterwards
  trusts its public half (`build_rust.sh` passes it to cargo as
  `THISTLE_APP_PUBLISHER_KEY`; `signing_verify_app`). The boot log says
  `Apps signed by this build's publisher key ... are trusted too`.
- **A packaged app is a shared object** built by `app_sdk/` (`-shared`, entry
  `thistle_app_main`), as Espressif's loader takes them; the `-r` link it
  used before put every section at address 0, and the loader could place
  none. The boot scan loads it into PSRAM, calls `thistle_app_main` for its
  `thistle_app_t`, and registers its callbacks, which run on the UI task as
  a built-in app's; the ELF stays loaded. An ELF entered anywhere else is
  refused (`enters_at_app_main`). It may link the `thistle_*` ABI and the C
  library functions in `APP_LIBC` (`syscall_table.rs`). The vendored loader
  (`components/elf_loader/vendor`) is patched to align each data section:
  it packed them, which left an app's table of callbacks misaligned.
- **A packaged app's files are `/sdcard/data/apps/<id>/`** on a device, as
  in the simulator (`APP_STORAGE_BASE`): under `/spiffs` no app could save,
  as SPIFFS has no folders and 31 bytes of path.
- **Everything a packaged app hands the kernel must be static or
  `thistle_malloc`'d**: the kernel refuses pointers into internal RAM, where
  the UI task's stack is (`syscall_is_ptr_allowed`). Leaving an app destroys
  it, so its `on_destroy` deletes its widgets; `lvgl_wm_widget_destroy()`
  forgets the handles of a deleted widget's children.
- **Every download follows redirects**, as a GitHub Release asset's URL
  answers with a 302 to another host: the catalog, a package and its
  signature, a firmware. At most five, never from HTTPS to HTTP
  (`redirect_policy.rs`, host-tested); the log says `Download refused:
  more than 5 redirects, to ...` when not. The kernel follows them itself
  (`thistle_http_client_init_download`, `thistle_http_client_redirect` in
  `http_client_shim.c`): ESP-IDF's own following has no scheme rule. What
  is installed is trusted for its signature and hash, never its host.
- **Refresh and Install run on the App Store's worker** (`appstore_job.c`),
  one at a time, on a task with a 16 KB internal stack, never on
  `lvgl_task`: its 8 KB stack once took a refresh's 100 KB of catalog
  entries, and the board crashed. An install's deepest point is Ed25519
  verifying the package's ELF, 7 to 8 KB, inside `install_tap_with()`; with
  a 4 KB copy buffer on the stack too, 12 KB overflowed. The device is held
  awake meanwhile; a job outlives the app's screens and says how it went in
  a toast.
- **Each list is one `lv_table`**: fourteen installed apps, as rows of
  widgets, ran LVGL's pool dry.
- The simulator's card is `/tmp/thistle_sdcard` to the kernel
  (`card_root()`). `simulator/tests/test_appstore.py` runs the app against a
  local catalog host; the simulator trusts only the production key, so an
  install there stops at the signature.

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
`lv_draw_add_task`, not a hang. A Messenger bubble costs about 0.75 KB even
with shared styles, which is why a conversation keeps 20 messages.

### The e-paper refresh

LVGL draws into the panel driver's framebuffer, and the panel refreshes once
nothing has been drawn for 60 ms, or a second after the first change if
something keeps drawing. While a finger is on the screen or Enter is held it
waits 400 ms, LVGL's long press, and 100 ms after either lets go: what is
drawn meanwhile is a button pressed, black, and shown at once every tap
flashed it (`epaper_refresh_settled()`,
`simulator/tests/test_epaper_refresh_policy.c`).
On a T-Deck Max a key press shows in about 0.6 s: the panel's fast waveform
is 430 ms of it, the two 9.6 KB frames 41 ms of SPI at 4 MHz, power-on 40 ms.
It took 0.95 s with a fixed 300 ms wait and 2 MHz, and a drag 1.5 s more.

- **The refresh runs on a task of its own** (`epaper` in `manager.c`, 3 KB
  stack), handed each refresh by `lvgl_task`, which reads input and draws on
  meanwhile; a tap that came and went during a refresh used to be lost. The
  GDEQ031T10 driver's `fb_lock` keeps a flush and a refresh off the
  framebuffer at once; a driver for another e-paper panel needs the same.
- **On e-paper an animation that plays once ends at once**
  (`__wrap_lv_anim_start`, by `--wrap` as for Arabic text), and a flicked
  list stops where the finger lifts (`scroll_throw` 100): their frames were
  never seen, only waited for. One that repeats, such as a blinking cursor,
  still runs, and costs a refresh each time it changes the screen.
- A board that saved its profile with the panel at 2 MHz gets 4 MHz at load
  (`normalize_driver_config`); any other clock is kept.

### Rust toolchains

Two toolchains, for two different jobs:

- **`esp`** — the Xtensa fork, for anything targeting the device (`cargo +esp check`, `cargo +esp build`). Installed by `espup`, *not* by rustup.
- **host stable** — for the kernel test suite, which runs natively. Pass your own host triple to `--target`; the crate's `.cargo/config.toml` defaults to `xtensa-esp32s3-espidf`, so an unqualified `cargo test` cross-compiles and dies with `'esp32s3' is not a recognized processor for this target`.

Install or update the `esp` toolchain with:

```sh
cargo install espup --version 0.16.0 --locked --force
espup install
```

**Pin espup to 0.16.0.** 0.17.x aborts with `Unsuported file extension: 'part'`, and CI pins the same version (`.github/workflows/ci.yml`) — keep the two in step.

The `esp` toolchain must be recent enough for the dependency tree: `cargo +esp check` fails at manifest-parse time with `feature 'edition2024' is required` on anything older than Xtensa Rust 1.85. espup 0.16.0 currently installs 1.97.0.0. If you see that error, your `esp` toolchain is stale — reinstall with the commands above rather than editing dependency versions.

Do **not** source `~/export-esp.sh` in the shell you run `idf.py` from — it overrides ESP-IDF's GCC with an incompatible version and breaks the C build. `components/kernel_rs/CMakeLists.txt` writes a throwaway script that sources it in an isolated subshell for the cargo invocation alone, which is exactly why the firmware build works without it in your environment.

## Architecture (three-tier immutable trust chain)

```
Recovery (Rust, `recovery` slot, 2.5MB) — reserved, not yet flashed (ADR-0010)
Kernel (100% Rust, `kernel_a` / `kernel_b`, 4.5MB each) — hardware-independent
  ↑ signed artifact manifest checked, then written into the other kernel slot
  ├── Display Server (surfaces, compositor, input routing)
  ├── Kernel modules (57 modules: app manager, IPC, events, permissions, signing, manifest, crypto, HAL registry, drivers, WM, app store, ...)
  ├── Syscall table (~80 syscalls exported to loaded ELFs)
  └── HAL registry (bus handles, driver vtables)
         ↓ loads from
SPIFFS (4.3MB) + SD card — updateable
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
- **Apps**: 16 built in, and 4 from the App Store (Notes, Weather, WiFi
  Scanner, Flashlight: `standalone_apps/`). Five remain in
  `components/apps_builtin/`; the other eleven are their own components under
  `apps/`, listed once in `cmake/thistle_apps.cmake` for the simulators and
  found by `EXTRA_COMPONENT_DIRS` for the firmware, which excludes the four
  store apps' old LVGL versions still there. The app manager has 32
  slots (`APP_SLOTS_MAX`), the built-in apps taking theirs at boot before any
  installed `.app.elf`; an app past the last is refused, which
  `test_app_matrix.py` shows as one that does not launch.
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
thistle-tk. All sixteen built-in apps are reachable by default on every S3
board; naming `thistle-tk` trades fourteen of them for its refresh behaviour.

`simulator/tests/test_app_matrix.py` launches every app on every simulated
device profile, and the simulator's `--wm NAME` overrides `system.json` for a
single run.

## Agent skills

### Issue tracker

Issues and specs live as markdown files under `.scratch/<feature-slug>/`. See `docs/agents/issue-tracker.md`.

### Domain docs

Single-context: `CONTEXT.md` + `docs/adr/` at the repo root (created lazily as needed). See `docs/agents/domain.md`.
