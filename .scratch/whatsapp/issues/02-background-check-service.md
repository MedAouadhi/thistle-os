# 02: Background check service

**What to build:** Any kernel module can register a **background check**
(ADR-0014, `CONTEXT.md`): a name, an interval it can change, the internal
RAM it needs, and a function. The kernel runs a due check on a FreeRTOS
task made for it (`kernel_task::spawn`) and gone after, holding the device
awake meanwhile. It tells the power manager when the next check falls, so
light sleep wakes on a timer for it; deep sleep runs none. A check returns
done, skipped and why, failed, or stopped until a time. The decisions are
a pure policy, host-tested, as `update_policy`'s and `power_policy`'s are.
The service is BSD and knows nothing of WhatsApp. Packaged apps cannot
register checks.

**Blocked by:** None (can start immediately)

**Status:** in-review; a check from light sleep is yet to be seen on hardware

- [x] Host tests: a check is due once its interval has passed; skipped with no network, with the clock unset, or with too little internal RAM free; not run while paused or stopped until a time; an interval of 0 is off
- [x] Host tests: the next wake the power manager is given is the earliest due check
- [x] A due check runs on its own task, holds the device awake, and the task is gone after
- [ ] A light-sleeping device wakes on the timer for a check, runs it, and sleeps again
- [x] Each run is logged with its name, its outcome, how long it took and the internal RAM free
- [x] A simulator-only check registered by an environment variable shows it running, in a simulator test

## Comments

**2026-11-01, the service built.** `background_check_policy.rs` decides
(16 host tests), `background_check.rs` runs it; the C API is
`thistle/background_check.h`. No new ESP-IDF call was needed: the task is
`kernel_task::spawn`'s, and the timer wake is the power manager's existing
`power_platform_light_sleep()` with its timeout.

- A due check waits up to 90 s (`NETWORK_WAIT`) for a network and the
  clock before it is skipped, which is how long a device woken from light
  sleep needs to bring WiFi or LTE back. Too little internal RAM skips it at
  once. Skipped or failed, it runs again an interval later, never sooner.
- The power policy (9 new host tests) sleeps until the earlier of deep
  sleep and the next check. Woken with a check due, it raises only the
  network (`RaiseNetwork`: `net_coordinator::rest(Normal)`), the screen
  and the drivers left asleep; once no check is due or running it rests
  the network again (`RestNetwork`) and waits for it, up to
  `SETTLE_TIMEOUT`, before the next light sleep. `CHECK_WAKE_MAX`, 5
  minutes, caps a wake. Deep sleep due meanwhile goes ahead; `wind_down`
  now rests the network even when the rest it wants is the one sleep had
  asked for, since a check may have raised it.
- The simulator's board does not sleep, so box 4 is the device's: the
  board was not connected. To see it, build with a test check registered
  (or the WhatsApp check of ticket 05), let the T-Deck Max sleep on
  battery, and look for `up for a background check`, the check's own
  `check NAME: ...` line and `background checks over; sleeping again` in
  the log, then the next sleep. Light sleep takes the USB-Serial-JTAG down,
  so the log is read after a key wakes it, or from the card.
- `simulator/tests/test_background_check.py` (7 cases, in `tests.yml`):
  `THISTLE_SIM_BACKGROUND_CHECK=SECONDS[:OUTCOME]` registers `sim`, which
  logs the task it runs on, how many tasks of that name are alive (1, so the
  last run's task is gone) and the hold it runs under.
