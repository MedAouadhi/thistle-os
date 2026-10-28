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

**Status:** done, but for box 5: an app update seen on the device

- [x] No wan0net reference remains outside history, licences and a line
      crediting the upstream project.
- [x] CLAUDE.md's section covers releasing (tags, the release environment,
      prereleases, raising the security version), the catalog, the kernel
      slots, the daily check, going back, card installs and the simulator's
      fakes; the App Store section no longer names upstream's catalog.
- [x] Each item of the hardware checklist is recorded in this ticket's
      Comments with what was seen: the USB migration, a real download
      through GitHub's redirect, Restart later, a kernel reset within its
      first minute rolled back, Go back, a card install, the power manager
      after the keep, and internal RAM around a check.
- [x] `v0.6.0` is published, immutable and attested, and a T-Deck Max on a
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

**2026-10-27, the hardware pass on the T-Deck Max.** rc.1's run published
nothing (43f4a4d); `v0.6.0-rc.2` is published, immutable and attested, its
signature checked against `tools/production_public.key`. The board ran a
USB-flashed rc.1 (security floor 0) with a card catalog naming rc.2, as the
Pages catalog leaves prereleases out. Logs were captured over USB.

- **The USB migration**: one `idf.py flash` of a laptop build moved the board
  to the two-slot table and the fork's key, keeping WiFi and settings
  (verified by the user on the device before the release).
- **A card install, tampered**: one byte of the image flipped: `card update
  refused: the file's SHA-256 is not its manifest's`; `update task (Card):
  6236 of 16384 bytes of stack never used`. The boot choice was left alone.
- **A card install, then a reset within the first minute**: `installed
  0.6.0-rc.2 into kernel_b`, booted from 0x4a0000, `running kernel_b,
  version 0.6.0-rc.2, new: kept after 60 s`, the board profile rewritten
  from rc.2's copy; reset before the keep, the board came back on rc.1 (the
  screen's version; the port moved during the reset, so that boot's log
  was lost).
- **A real download through GitHub's redirect**: it failed at first,
  `download failed 0xFFFFFFFF`, with `HTTP_CLIENT: Out of buffer` after
  `Redirected (1): https://release-assets.githubusercontent.com/...`: the
  signed URL is about 970 characters, the request buffer ESP-IDF's 512.
  Fixed in a2bb096 (2 KB for downloads). With it the manifest, signature
  and image each followed one redirect, `installed 0.6.0-rc.2 into
  kernel_b; it runs after the next restart`, `update task (Download): 6732
  of 16384 bytes of stack never used`. **rc.2 itself has the bug**, so it
  cannot download a later release; `v0.6.0` has the fix.
- **Restart later, then the keep and the power manager**: Later, then a
  restart by hand: `running kernel_b, version 0.6.0-rc.2, new: kept after
  60 s`, `the power manager starts once this kernel is kept`, `kept the
  kernel in kernel_b after 60 s; starting the power manager`, `security
  floor raised from 0 to 500`. The next boot: `security floor 500`, not new.
- **Go back**: Settings offered `Go back to 0.6.0-rc.1`; `going back to
  0.6.0-rc.1 in kernel_a; restarting`, booted from 0x20000 new, its own
  board profile back, `kept the kernel in kernel_a after 60 s`, the floor
  still 500.
- **Internal RAM**: 132 to 134 KB free, largest block 54 to 60 KB, at every
  boot of the pass; the App Store's Refresh, a check, left 10.8 KB of its
  worker's stack unused.
- The card was put back as it was afterwards.

Not yet seen: a download on battery alone (the user's earlier one failed,
most likely from the same buffer), and boxes 4 and 5, which wait for the
`v0.6.0` tag and an app tag.

**2026-10-28, `v0.6.0` and the first app releases.** Tagged on 4212a7d
with the user's approval: published, immutable, attested; SHA256SUMS,
the manifest's signature against `tools/production_public.key` and the
attestation check out, and the catalog offers it. The T-Deck Max, on
battery (no log), went back from rc.2 to the fixed rc.1 build, was
offered 0.6.0, downloaded and installed it over the air, and runs it.
On rc.2 the same download failed with 0xFFFFFFFF, as expected: a device
on rc.2 moves by the card, Go back or USB (the CHANGELOG says so).

The four store apps were released as `notes-v1.0.0`,
`weather-v1.0.0`, `wifiscanner-v1.0.0`, `flashlight-v2.0.0`, then
`notes-v1.0.1`. Three findings, fixed:

- **A push of more than three tags starts no workflow** (GitHub's rule):
  the four were deleted and pushed one at a time. CLAUDE.md says so.
- **Pages kept the previous catalog** when the app releases, tagged on
  4212a7d like `v0.6.0`, deployed theirs: the deploy reported success,
  the site still served the 19:59 files. Pages keeps what it has for a
  build version it has seen, the commit by default. `notes-v1.0.1`, on
  a new commit, published the catalog with all four; `pages.yml` now
  gives each run a version of its own.
- **GitHub marked `weather-v1.0.0` the Latest release** over `v0.6.0`:
  app releases now publish with `--latest=false`.

Box 5 waits on the device: Notes 1.0.1 shown on Home and in the
Installed tab as an update to an installed Notes, and installed.
