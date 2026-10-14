# 17: Keep the clock set, in the user's time zone

**What to build:** The T-Deck Max shows the right local time without anyone setting it: from a time server over WiFi or mobile data, from the GPS while it has a fix, or from the mobile network, and from a clock chip on boards that have one. Settings shows where the time came from, picks the time zone and the 12- or 24-hour clock, and lets the user set the time by hand with automatic time off.

**Blocked by:** none.

**Status:** in-review; seen on hardware but for GNSS

- [x] A kernel clock (`time_manager.rs`) that ranks its sources, NTP, GNSS, the mobile network, a clock chip, and decides in `time_policy.rs`, tested on the host (ADR-0007)
- [x] NTP from `pool.ntp.org` (`thistle_os.time.ntp_server`), hourly and whenever WiFi or PPP gets an address; its answers judged like the others' by replacing ESP-IDF's weak `sntp_sync_time()` (`time_platform.c`)
- [x] Small corrections slewed, large ones stepped; nothing before 2026 believed
- [x] GNSS time taken from the driver's last fix, only while it moves with the kernel's clock
- [x] The A7682E's clock (`AT+CTZU=1`, `AT+CCLK?`) read after registration and at each SMS fetch, through a new `get_network_time` at the end of `hal_net_driver_t`; parsed and tested on the host
- [x] The PCF8563 can be named in a board profile (`com.thistle.drv.rtc-pcf8563`); read at boot, rewritten after a better source sets the clock. Its comment no longer puts it at 0x55 on the T-Deck Pro
- [x] What set the clock, and the mobile network's offset, kept in RTC memory through restarts and deep sleep
- [x] Time zones: about 120 IANA zones as POSIX rules (`time_zones.rs`), each checked against its tzdata offsets in winter and summer; or Automatic, the mobile network's offset
- [x] Settings → Date & time: the time and what set it, automatic on or off, the time typed in by hand, the zone (regions, then cities), 12 or 24 hours
- [x] The status bar, Messenger, Assistant and the thistle-tk launcher take the time from `time_manager_format_time()`, in local time; the WiFi manager's NTP stub and UTC clock are gone
- [x] Verified on hardware: NTP over WiFi and LTE, the modem's time and offset, every zone's rule in newlib, deep sleep (below)
- [ ] Verified on hardware: GNSS, which needs a fix the bench has never had

## Comments

### Sources, as they behave

The Max has no clock chip and no 32 kHz crystal, so after a power-on the status bar says `--:--` until a source answers. Light sleep, deep sleep and restarts keep the time.

- **NTP** answers within seconds of an address, after a random wait of up to 5 s (`CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY`), and again every hour.
- **GNSS** counts only while something has the receiver on (Navigator, a GPS track) and it has a fix; the time manager needs two looks 30 s apart, its time moving with the kernel's, before it believes it. It corrects the clock only when NTP has not been heard for an hour.
- **The mobile network** answers when the modem registers, if the network sends NITZ at all, and at each SMS fetch: every two minutes while listening without data, and seldom during a data session, when NTP over LTE answers instead.

### Settings

`thistle_os.time` in system.json: `automatic` (default true), `zone` ("" for Automatic, the default, or an IANA name; a POSIX rule written by hand is applied as is), `format_24h` (default true) and `ntp_server` (default `pool.ntp.org`, not in the UI).

The zone picker builds its rows from two shared LVGL styles, with Date & time closed while it is open. Built as the other Settings pages are, one property at a time, its region list ran LVGL's 64 KB pool dry, and LVGL's malloc assert spun inside the flex layout: the simulator hung. On the simulator Europe's 33 cities leave 19 KB of the pool free.

### Simulator

The host's clock is the system clock and nothing changes it; starting NTP reports a sync from the host at once, and the simulated modem gives the host's time and offset once registered. `--device tdeck-max --launch-app com.thistle.settings --tap 120,219` opens Date & time.

### Seen on hardware

2026-10-13, on the T-Deck Max with WiFi and LTE up and a SIM in, by a self-test build (not kept) that surveyed the zones through newlib, compared the modem's and the GPS's times with the clock, then asked for deep sleep with a 20 s timer and reported after it. Rust log lines on the device carry no tag and no newline (`esp_log_write` under log v1, the same for every Rust module), so they run into the next line.

```
clock not set yet; zone Automatic (UTC), UTC now             after the flash
NTP from pool.ntp.org                                9.47 s
WiFi got IP / clock set by NTP                       ~14 s   4.5 s after the address
mobile data: connected / clock slewing by -6 ms, from NTP    NTP asks again on PPP's address
zone Automatic (UTC+02:00), UTC+02:00 now, from the mobile network
```

- **The modem's time**: `AT+CCLK?` read after registration gave +02:00 (CEST, right) and a time 0.21 s from NTP's, within the second it counts in. Whether `AT+CTZU=1` or an earlier setting turned NITZ on cannot be told apart, since the module keeps it; the module's offset is the network's.
- **Zones**: all 120 rules, read by newlib on the device and glibc on the host, give the same offset changes through 2026 to the second, and mktime() the same UTC for noon local on 15 January and 15 July: identical output. Asia/Dubai (`<+04>-4`), Asia/Jerusalem (`M3.4.4/26`, summer from Friday 27 March 02:00) and Pacific/Chatham (`<+1245>-12:45...`) among them. The survey took 997 ms.
- **The clock against the host's**, both by NTP: the device's lines arrived 0.07 to 0.25 s after the time they carried, which is the console's latency.
- **Deep sleep**, twice, 20 s on a timer: the boot after it said `2026-10-13 18:29:27 UTC, from NTP` and Settings would have said "Set by NTP, 5 min ago". The first time, an automatic zone came back as UTC until the modem was read again, 15 s later; the record now keeps the network's offset too, and the second time the boot line said `zone Automatic (UTC+02:00), UTC+02:00 now`.
- **Drift**: NTP after each wake slewed by -125 ms and -142 ms. The RTC runs on the S3's internal 136 kHz RC oscillator (`CONFIG_RTC_CLK_SRC_INT_RC`, ESP-IDF's default), whose rate moves with temperature; at that rate a day of deep sleep would leave the clock minutes out until a source answers. ESP-IDF's `CONFIG_RTC_CLK_SRC_INT_8MD256` is the steadier internal source, for about 5 uA more in deep sleep; not tried.
- **A reflash** kept the time but not well: after it, and two resets, the clock was 3.9 s out, and NTP slewed it back. The record, whose layout had changed, was rejected by its CRC: `from before the restart`, as meant.
- **GNSS**: the receiver talked at 38400 baud and reported no satellites indoors, so its time was never offered. Not verified.

- **The reset button** (S1, EN) loses the time as a power-on does: EN resets the RTC domain, record included. The firmware without the self-test, flashed after it, booted with `clock not set yet; zone Automatic (UTC), UTC now` and `NTP from pool.ntp.org`, and waited: WiFi, mobile data and SMS had been switched off in its Settings by then.

Not looked at: Settings → Date & time on the device's own screen (only in the simulator).

### Seen beside it

After the second wake the board slept at its 120 s idle time with USB plugged in: "Stay awake when plugged in" took USB from the charge state, which reads "not charging" for a full battery whose charging has paused. Fixed under ticket 13 ("A plugged-in board that slept").
