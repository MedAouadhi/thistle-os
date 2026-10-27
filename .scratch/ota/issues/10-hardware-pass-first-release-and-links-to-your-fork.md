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

- [x] No wan0net reference remains outside history, licences and a line
      crediting the upstream project.
- [x] CLAUDE.md's section covers releasing (tags, the release environment,
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

## Comments

**2026-10-26, implementer: the documentation part.** Nothing pushed, no
tag created.

- **Links.** The docs' pages, the site, the WASM shell, README,
  thistle-tk's README, `docs/app-package-format.md` and every
  `standalone_apps/*/metadata.json` point at github.com/MedAouadhi/thistle-os
  and https://medaouadhi.github.io/thistle-os/ (README's thistle-tk link is
  now `crates/thistle-tk/`; its board link, to a page that never existed,
  is the devices page). The example and simulator fixture catalogs and
  `redirect_policy.rs`'s test URL name the fork too. README's License
  section has the one line crediting upstream.
- **Left naming wan0net, on purpose**: `.scratch/`, `docs/adr/`,
  `docs/architecture-conversation-record.md`, `FABLE_IMPROVEMENTS.md` (records),
  `docs/rust-driver-inventory.md` (links to upstream's issues #120 to #127,
  where that work is tracked), and Recovery: `recovery/` and the two URLs
  `docs/recovery.html` describes, out of scope per the spec until Recovery is
  brought up. `tools/test_upstream_links.py` (in `tests.yml`) fails on any
  other, and lists the allowed paths with their reasons.
- **Docs.** `docs/release-flashing.html` is rewritten for the tag-based flow:
  tags, the `release` environment, prereleases, the CHANGELOG section,
  raising the security version, the assets and how to verify them, over the
  air, the card's three files, the full flash image for a new device (it
  erases NVS, as `merge_bin` fills the gaps), and moving a device over with
  one `idf.py flash`. `docs/security.html`'s OTA section and
  `docs/app-store.html`'s firmware and default-catalog sections follow.
- **CLAUDE.md.** "### Two kernel slots" became "### Releases and updates",
  with releasing, the key, versions, the security version and floor, the
  catalog, the slots, the daily check, download and install, the keep, Go
  back, card installs, USB, and the simulator's variables in one bullet.
  The catalog's description moved there from the App Store section, which
  now points at it and no longer names upstream's key either. The
  Architecture diagram was already right. The CI line names the release
  workflows and the new test.
- **CHANGELOG.md** has `## v0.6.0-rc.2 - 2026-10-27` (rc.1's run stopped before publishing: the downloaded firmware was not where Stage the release looked; fixed in 43f4a4d);
  `release_firmware.changelog_notes` reads it.

Tests: every `tools/test_*.py` that `tests.yml` runs passes;
`redirect_policy.rs`'s tests pass.

Left for the user: boxes 3 to 5, the hardware pass and the tags. Before
`v0.6.0`, add its own `## v0.6.0 - <date>` section; and if not done yet, add
tag rules `v*` and `*-v*` to the `github-pages` environment (ticket 04), or
a release's Pages deploy is refused. The spec's Status line waits for those.
