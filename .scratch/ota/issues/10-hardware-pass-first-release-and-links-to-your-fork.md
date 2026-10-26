# 10: Hardware pass, the first release, and every link to your fork

**What to build:** The fork's first real release, proven on the T-Deck Max,
with the documentation saying how it all works. Every wan0net link in the
docs, the site, the README and the apps' metadata points at the fork, and the
release and flashing docs describe the tag-based flow. CLAUDE.md gains a
"Releases and updates" section in the manner of its other feature sections.
Then the hardware checklist from the spec's Testing Decisions is run end to
end, `v0.6.0` is tagged, and one app release follows it. Spec: `../spec.md`,
"Testing Decisions" and "Further Notes"; the spec's Status line is updated
when this is done.

**Blocked by:** 03 (Two kernel slots), 06 (Firmware installs check the
artifact manifest and the security floor), 07 (Update over the air from the
catalog), 08 (Go back, and the board profile follows the firmware version),
09 (App updates and the Home row)

**Status:** ready-for-agent

- [ ] No wan0net reference remains outside history, licences and a line
      crediting the upstream project.
- [ ] CLAUDE.md's section covers releasing (tags, the release environment,
      prereleases, raising the security version), the catalog, the kernel
      slots, the daily check, going back, card installs and the simulator's
      fakes; the App Store section no longer names upstream's catalog.
- [ ] Each item of the hardware checklist is recorded in this ticket's
      Comments with what was seen: the USB migration, a real download
      through GitHub's redirect, Restart later, a kernel reset within its
      first minute rolled back, Go back, a card install, the power manager
      after the keep, and internal RAM around a check.
- [ ] `v0.6.0` is published, immutable and attested, and a T-Deck Max on a
      laptop build of the fork is offered it, installs it over the air and
      keeps it.
- [ ] An app release after `v0.6.0` shows on that device's Home and installs
      from the Installed tab.
