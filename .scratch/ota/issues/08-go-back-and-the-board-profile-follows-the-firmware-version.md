# 08: Go back, and the board profile follows the firmware version

**What to build:** Two things that let a firmware version and its board
profile travel together. First, the stored board profile records the
firmware version that wrote it; when a kernel boots and that version is not
its own, and it embeds a profile for the stored board id, it rewrites the
stored profile from its embedded copy and keeps the previous one as a single
backup. A board id it has no profile for keeps its profile. This ends the
"changed profile never reaches a board that has one" trap tickets 03, 06, 09
and 10 of the T-Deck Max hit, for USB flashes and updates alike. Second,
Settings → System update offers Go back to the previous version, with no
download, while the other kernel slot's record matches that slot's own image
description and its security version is at or above the floor; going back
boots that slot, and its kernel brings back its own board profile. Spec:
`../spec.md`, "The update manager". ADR-0010.

**Blocked by:** 06 (Firmware installs check the artifact manifest and the security floor)

**Status:** ready-for-agent

- [ ] `update_policy` decides, tested on the host, whether the board profile
      is rewritten (writer version equal, different, missing; board id with
      and without an embedded profile) and whether Go back is offered (record
      matching and not matching the image, below and at the floor).
- [ ] Simulator tests: a version change rewrites the profile and leaves a
      backup; a custom board id keeps its profile; Go back boots the other
      slot and the profile follows it; Go back disappears once a new install
      starts erasing the other slot.
- [ ] On the T-Deck Max, a laptop build with a changed embedded profile
      reaches the board on its first boot, with the old profile kept beside
      it, and system.json, WiFi and the mesh identity untouched.
- [ ] On the T-Deck Max, after an update, Go back returns to the previous
      version.
- [ ] CLAUDE.md's "A changed board profile does not reach a board that has
      one" section says what happens now.
