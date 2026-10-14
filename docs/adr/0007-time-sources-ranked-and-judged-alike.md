# Time sources are ranked, and NTP is judged like the rest

Nothing set the clock. `wifi_manager_ntp_sync()` logged "Starting NTP sync..." and returned, the PCF8563 driver could not be named in a board profile, and the status bar showed UTC worked out by hand. The T-Deck Max has no clock chip to fall back on: 0x55 on its bus is the BQ27220 fuel gauge, not an RTC (T-Deck-MAX `docs/pinmap.md`, section 13), and GPIO15 and 16, where a 32 kHz crystal would go, carry the keyboard's INT and the GPS's TX. What it has is three ways to learn the time: NTP over WiFi or LTE, the MIA-M10Q's fix, and the time the mobile network gives the A7682E (NITZ).

We added one place that decides, `time_policy.rs`, and one that acts, `time_manager.rs`. Sources rank NTP, then GNSS, then the network, then a clock chip. Any source sets a clock that reads before 2026. After that, a worse source is heard only when nothing better has confirmed the clock for an hour, and only if it disagrees by more than it is itself good to (2 s for whole-second sources). A correction of up to 10 s is slewed with `adjtime()`, which ESP-IDF runs at 1/64, so the clock never jumps back for a small one; a larger one is stepped. With automatic time off, only the user sets the clock, and a clock chip after a power loss.

NTP goes through the same judgement. ESP-IDF declares `sntp_sync_time()` weak so that an application can apply the time itself (`esp_sntp.h`); `time_platform.c` replaces it and hands each answer to the kernel.

The zone is a POSIX rule in `TZ`, chosen by IANA name from a table of about 120 zones (`time_zones.rs`), or taken from the network's offset.

## Considered Options

**ESP-IDF's smooth mode** slews any NTP correction under about 35 minutes at 1/64. A clock half an hour out, as after long deep sleep on the RC oscillator, would take 32 hours to come right. **Its immediate mode** steps the clock every hour, backwards by milliseconds as often as forwards, and would still leave the other sources to decide on their own terms. Neither would know that automatic time is off.

**NTP through the modem** (AT+CNTP) needs the module in command mode, which a data session does not allow (ADR-0004). PPP carries ordinary NTP anyway.

**A zoneinfo database** would cover every zone and its history, but newlib reads only POSIX rules, and the device has no use for history: the clock only ever shows now. The table's rules are the ones tzdata ends each zone's file with, and each is tested on the host against that zone's winter and summer offsets.

**The GPS driver's callback** fires on every fix, but holds a single subscriber, and the clock taking it would leave none for anything that tracks position. The time manager reads the driver's last position every 30 s instead, and believes it only when its time has moved on by as much as the kernel's own clock: a receiver whose sentences stop keeps its last time.

**Saving the last known time to flash**, as fake-hwclock does on a Raspberry Pi, would give a device that lost power a time close to right. It would also show that time as if it were right. MeshCore keeps its own forward-only clock for its replay protection, and TLS here does not check certificate dates (`CONFIG_MBEDTLS_HAVE_TIME_DATE` is off), so nothing needs a guess; the status bar shows `--:--` until a source answers.

## Consequences

A board without a clock chip loses the time at a power-on, and has it back once a network or a fix is there. Through light sleep, deep sleep and a restart, ESP-IDF keeps it (`CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT`), and `time_platform.c` keeps what last set it in RTC memory, so that Settings can still say "Set by NTP, 3 h ago" after deep sleep. The same record keeps the mobile network's last offset, so that an automatic zone does not show UTC from a wake until the modem is next read.

`hal_net_driver_t` has a new last field, `get_network_time`, so no field before it moves. The A7682E driver reads `AT+CCLK?` after it registers and at each SMS fetch, never by interrupting a data session; during one, NTP over LTE does the job.

An automatic zone is a fixed offset. It follows a change to or from summer time only when the modem's clock is next read, every two minutes while the modem is listening without data, and less often during a data session. A country that changes its rules needs its line in `time_zones.rs` changed; Morocco's hour back for Ramadan, which tzdata lists year by year, has no POSIX rule at all.
