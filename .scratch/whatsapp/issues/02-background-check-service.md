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

**Status:** ready-for-agent

- [ ] Host tests: a check is due once its interval has passed; skipped with no network, with the clock unset, or with too little internal RAM free; not run while paused or stopped until a time; an interval of 0 is off
- [ ] Host tests: the next wake the power manager is given is the earliest due check
- [ ] A due check runs on its own task, holds the device awake, and the task is gone after
- [ ] A light-sleeping device wakes on the timer for a check, runs it, and sleeps again
- [ ] Each run is logged with its name, its outcome, how long it took and the internal RAM free
- [ ] A simulator-only check registered by an environment variable shows it running, in a simulator test

## Comments
