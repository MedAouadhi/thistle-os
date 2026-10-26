# 06: Firmware installs check the artifact manifest and the security floor

**What to build:** A release copied to the card (its `.bin`, artifact manifest
and the manifest's signature in `/sdcard/update/`) installs from Settings →
System update, and anything else is refused with the reason shown. A
firmware is accepted only if the manifest's signature verifies against the
kernel's key, its type is firmware, its arch is the running chip's, the
stored board id is among its compatible boards, its security version is at
least the device's security floor, and the bytes written match its size and
hash. Each kernel slot's version and security version are recorded in NVS
when it is written, and by a kernel for its own slot when it runs; the floor
rises when a new kernel is kept, so one rolled back never raises it. The
manifest's parsing and device checks come out of Recovery's module into a
shared crate the kernel uses; the decision is `update_policy`'s, pure and
tested on the host. This replaces the card update's detached `.sig` check.
Spec: `../spec.md`, "The artifact manifest", "The update manager", "Device UI"
and "Simulator". ADR-0010.

**Blocked by:** 03 (Two kernel slots), 04 (Release the firmware by tag)

**Status:** ready-for-agent

- [ ] The shared crate carries the canonical-format tests Recovery's module
      has; Recovery itself is not changed.
- [ ] `update_policy`'s accept decision is tested on the host: bad signature,
      wrong type, wrong chip, board not listed, below, at and above the
      floor; a card install may be any version at or above the floor.
- [ ] Settings → System update shows the running version, Install from card
      when the card holds a release, progress, Restart now / Later, and the
      reason for a refusal; Settings' old SD apply is gone.
- [ ] The simulator trusts a test publisher key named by an environment
      variable, in simulator builds only.
- [ ] A simulator test installs from the card and refuses a bad signature, a
      wrong board, a wrong chip, a version below the floor and a size
      mismatch, each with its reason in Settings and the log; the floor rises
      only after the keep, and not for a run ended before it.
- [ ] On the T-Deck Max, a release candidate cut after this ticket installs
      from the card, and a copy with one byte changed is refused.
