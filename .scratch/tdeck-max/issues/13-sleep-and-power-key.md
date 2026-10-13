# 13: Sleep, deep sleep and the power key

**What to build:** The T-Deck Max stops being always on. It sleeps after a spell nobody uses it, and goes into deep sleep after a longer one, both set in Settings. The power key sleeps it and opens a power menu. The power key, the keyboard and the modem's RI line wake it.

**Blocked by:** 10 (cellular data), for the modem's sleep.

**Status:** in-review

- [x] Sleep (light sleep, RAM kept) after a time set in Settings: never, or 1, 2, 5, 10 or 30 min; 5 by default
- [x] Deep sleep after a longer time: never, or 30 min, 1, 2, 6 or 24 h; 1 h by default
- [x] The power key: a press sleeps, holding it 1.5 s opens a menu (Sleep, Deep sleep, Restart, Cancel)
- [x] Settings → Power: both times, the two switches below, Sleep now, Deep sleep now, Restart
- [x] The power key and the keyboard wake it from either sleep; the key pressed to wake it is not typed
- [x] The modem's RI wakes it from sleep, and from deep sleep while "Calls and SMS wake it" is on (default on)
- [x] LoRa keeps receiving in sleep; a packet wakes the CPU for MeshCore and the device sleeps again
- [x] Nothing that sleep would break is cut off: an update being written, audio, a download or app install, an assistant answer, a GPS track, a serial session, Bluetooth, a call ringing
- [x] Boards that saved their profile before it had a power section get the section, and only it
- [x] Verified on hardware: sleep, wake, deep sleep and the boot after it, by a self-test build (below)
- [ ] Verified by hand: the power key and a key on the keyboard waking each sleep, the power menu, an SMS waking the device
- [ ] Measured: the current in each sleep

## Comments

### What the power key is

The T-Deck Max has three buttons (schematic, *T-Deck Pro Max V0.1 25-09-11*):

- **S2, IO0** (page 2), pulled up by R82 (10K): the BOOT button, active low. This is the power key.
- **S1, EN** (page 2): reset.
- **S3, the modem's BOOT_CFG** (page 5): the A7682E's download mode, which the SoC cannot see.

The charger has no button: the SY6970's QON (page 1, BQ25896 footprint) carries only R18, and its INT is not routed. So there is no "power off and back on by the key". IO0 is an RTC pad, so it can wake the SoC from deep sleep, and LilyGo's own factory firmware sleeps that way: `esp_sleep_enable_ext1_wakeup((1UL << BOARD_BOOT_PIN), ESP_EXT1_WAKEUP_ANY_LOW)` with the comment "Hibernate using user keys" (`examples/factory/ui_deckpro_port.cpp:633`). The keyboard is then a second wake source, not a fallback.

### The other wake pins

| Source | Pin | Level | Notes |
| --- | --- | --- | --- |
| Keyboard, TCA8418 INT | IO15 | low | 10K pull-up (page 7). The chip scans while the SoC sleeps; INT stays low until the event FIFO is read and acknowledged, so the driver empties it before sleep |
| Modem RI | IO7 | low | Through U17, an RS0104 level shifter whose OE is the modem's `VDD_EXT` (page 6): with the modem off, IO7 floats, so the power manager pulls it up |
| LoRa DIO1 | IO5 | high | Light sleep only; `ext1` takes one level for all its pins |

RI behaviour (A76XX Sleep Mode Application Note V1.03, 3.2; A7682E Hardware Design V1.05, 3.3.1): low 120 ms for an SMS, 60 ms for a URC with `AT+CFGRI=1` (left at its default of 0, so URCs do not wake the device), and low while a call rings. The module sleeps with `AT+CSCLK=1` and DTR high, still paging the network: LTE sleep 2 mA typical against 12 mA idle (Hardware Design table 35).

### Deep sleep

Waking from deep sleep restarts the SoC. Before it, the radio sleeps, the e-paper controller takes its deep-sleep command, and the profile's `deep_sleep_off` rails are cut: LORA_EN, GPS_EN, 1V8_EN, the motor and the amplifier (XL9555 P01, P02, P03, P05, P06). The expander holds its outputs through the SoC's deep sleep, and the boot sequence raises them again. The modem stays powered and asleep if "Calls and SMS wake it" is on, else it is shut down (AT+CPOF, then its rail). LilyGo measured the whole board at 0.34 mA with every rail off (`README.md`, "Test"); the modem asleep adds about 2 mA, so about 25 days on the 1400 mAh cell against about 170.

DTR is held high through deep sleep with `gpio_hold_en`, and the modem driver releases it at init, so the module wakes when the board does.

An RI wake from deep sleep boots the device and puts it back into deep sleep after 30 s unless someone uses it. An SMS wake from sleep does the same for 30 s, back to sleep. A call ringing holds RI low and keeps the device awake.

Nothing deep-sleeps before `ota_mark_valid()`: the power manager starts after it, at the end of `app_main`. A restart before the image is marked valid would roll it back.

### Choices made without the user

The request left these open, and they were settled on the recommended side, each easy to change:

- RI wakes from deep sleep behind a setting, on by default (the 2 mA above), rather than always or never.
- LoRa keeps listening in sleep (about 5 mA for the SX1262 in RX) rather than being switched off.
- An SMS gets a brief wake rather than a full one.
- "Stay awake when plugged in" is on by default. Light sleep turns off the S3's USB-Serial-JTAG (`sleep_modes.c`, `sleep_console_usj_pad_backup_and_disable`), so a board sleeping on a desk drops off the USB it is flashed through. The idle timers stop; the power key and "Sleep now" still sleep a plugged-in board.

### Light sleep on the S3, and what bit

- **`CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND` isolates every pad in light sleep.** It protects input-only pins from an ESD reset, but it also floats outputs: the LoRa, e-paper and SD chip selects, and DTR. `power_platform.c` keeps every pad driven as an output driven through the sleep, flash and PSRAM pads excepted.
- **A GPIO wake rewrites the pin's interrupt type to a level.** A driver's edge interrupt on that pin would fire without end once the level is reached. Each wake pin's configuration register is saved, its interrupt masked through the sleep, and restored after; a pin still at its level then has its interrupt status set, which replays the edge the sleep swallowed. That is how the SX1262's receive interrupt reads the packet that woke the CPU, with no change to its driver.
- **`ext1` routes its pads to the RTC domain**, and in light sleep nothing routes them back, which kills a digital interrupt on them. Light sleep uses GPIO wake; only deep sleep, which restarts, uses `ext1`, and the pads are handed back at boot.
- **Light sleep costs 9 KB of internal RAM.** Powering the CPU down in light sleep saves about 650 uA and keeps its context in internal RAM (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP`, help text). The board now boots with 150 KB free (largest block 72 KB) where it had 159 (80). Turning off `PM_RESTORE_CACHE_TAGMEM_AFTER_LIGHT_SLEEP` to save some of it does not compile on ESP-IDF 5.5.1 (`sleep_cpu.c`: `s_tag_mem` undeclared).
- **The log's timestamps stop during light sleep**: they count FreeRTOS ticks. A 10 s sleep shows as nothing between two lines.

### Verified on hardware

A self-test build (not kept) asked for sleep 25 s after boot, capped light sleep at 10 s and took the timer as a key press, then asked for deep sleep and added a 20 s timer to it. On the T-Deck Max, with LTE and WiFi up:

```
SELFTEST: asking for sleep / going to sleep        39.66 s   WiFi stopped, PPP ended
ui_mgr: refreshing panel (full)                    40.11 s   the sleep screen
a7682e: sleep: module asleep, RI raised ...        47.71 s
(light sleep, 10 s by the host's clock)
woken by a key / awake / a7682e: module awake      48.40 s
network back from sleep                            48.43 s   WiFi rejoins
ui_mgr: refreshing panel (full)                    48.78 s   the app again
a7682e_net: dialling                               50.58 s   PPP back
SELFTEST: asking for deep sleep                    88.49 s
a7682e: sleep: module asleep ...                   96.63 s
(USB gone; 20 s later the board boots)
a7682e: power-on: modem answering                   9.53 s   no PWRKEY press: it stayed powered
```

The modem dominates settling: leaving the PPP session (the `+++` escape and its guard times) and `AT+CSCLK=1` take about 8 s, within the 15 s the power manager waits.

Seen in that run and fixed: "WiFi disconnected", which stopping WiFi posts as a toast, could land on the sleep screen and stay on the e-paper. Toasts are dropped while the sleep screen is up.

Seen by the user and fixed: the front light and the key backlight stayed on under the sleep screen. They went out only when settling ended, after those 8 s, and a key pressed during settling left them out. Both lights (LEDC channels 0 and 1, IO41 and IO42, each switching an S8050 with a pull-down on its base, schematic pages 4 and 7) now go out with the sleep screen, and come back however the sleep ends. A second self-test read both duties back 5 ms after each change, at the saved 25%:

```
before sleep                63 / 63
going to sleep (settling)    0 / 0
asleep, before light sleep   0 / 0
woken, before the lights     0 / 0
awake                       63 / 63
```

### Not done

- Other boards: only the T-Deck Max profile has a `power` section. The T-Deck Pro has the same BOOT key and keyboard and would take the same section.
- A phone app: RI from a call wakes the device and holds it awake, but nothing answers the call.
- Sleep and wake events on the event bus, for apps.
- Under thistle-tk there is no sleep screen or power menu: holding the key goes straight to deep sleep.
