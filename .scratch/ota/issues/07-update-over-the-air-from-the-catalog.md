# 07: Update over the air from the catalog

**What to build:** The device finds and installs firmware releases on its own.
Once a day, when a network is up, it reads the catalog (at most once a boot
while the clock is unset); Settings → System update has Check now. It offers
the newest firmware entry that is newer than the running version and suits
its chip and board, with the release's notes. Download and install streams
the firmware from GitHub, through the redirect, into the other kernel slot
with the checks of ticket 06, holding the device awake and showing progress,
then asks Restart now or Later. On mobile data the download waits until the
user allows it. The check and its catalog are the App Store's too: one cached
catalog. The App Store's Firmware tab goes. The kernel's `update_manager`
runs it, with the decisions in `update_policy`, and Settings, Home and the App
Store read its state through a small C API. Spec: `../spec.md`, "The update
manager", "Device UI" and "Testing Decisions". ADR-0010, ADR-0011.

**Blocked by:** 05 (Release an app by tag and install it from your catalog),
06 (Firmware installs check the artifact manifest and the security floor)

**Status:** ready-for-agent

- [x] `update_policy` decides, tested on the host: which release to offer
      (newer, equal, older, a laptop build's base, wrong chip or board),
      whether a check is due (clock set and unset), and whether a download
      may start (WiFi, mobile data with and without consent).
- [x] A kernel worker job notices a check is due; the check and any download
      run on one FreeRTOS task made for them and gone afterwards, with an
      internal stack that never holds the catalog's entries.
- [x] The C API gives the state (idle, checking, up to date, available,
      downloading, writing, ready to restart, failed with a reason), the
      version on offer and its notes, and calls for check now, install
      (with mobile-data consent) and restart.
- [x] The simulator test runs against a fake GitHub (a catalog server, and an
      assets server behind 302s): the offer after a check, install, Restart
      later and the next run booting the new version, a size mismatch, a
      redirect to HTTP, six redirects, and mobile data waiting for consent.
- [x] The log says what each check found, what was offered, installed, kept
      or rolled back, and why anything was refused.
- [ ] On the T-Deck Max, with `config/appstore.json` pointing at a catalog
      that lists a release candidate, the device offers it, downloads it
      from GitHub and installs it; the boot log's internal RAM line is the
      same before and after a check.

## Comments

**2026-10-26, implementer.** Done with no board attached; nothing pushed,
no tag created. Ticket 08 is merged in.

- **`update_policy.rs`** gains `firmware_releases` (the catalog's flat
  firmware entries; only those objects are parsed into JSON values, as a
  whole catalog's tree of small allocations would land in internal RAM),
  `pick_offer` (newest newer than the running version, a laptop build
  counting as its tag; this chip and board; versions that do not read are
  skipped), `check_due` (a day after the last check, at once if none or the
  clock went back, once a boot with the clock unset, never within an hour
  of the last try, never without a network or while busy) and
  `may_download` (WiFi starts, mobile data waits for consent, no network
  waits). 18 new host tests.
- **`update_manager.rs`**: a kernel worker job every 2 s (`tick`) starts the
  daily check, or a download that waits once the network allows it. Every
  job (check, download, card install) runs on one FreeRTOS task with a 16 KB
  internal stack made for it and deleted after (`start_job`); the catalog
  (64 KB at most), its entries and the 4 KB copy buffer are on the heap,
  so PSRAM, never on that stack. The check keeps the catalog at
  `/sdcard/appstore/catalog.json` and records when and what in
  `/spiffs/update.json`; the App Store's Refresh counts as a check
  (`catalog_read`), and a restart reads the kept catalog's offer back. The
  download fetches the manifest and signature, `accept`s them with
  `Source::Online`, opens the image (`appstore_client::HttpStream`, which
  follows redirects through `redirect_policy` and says why one is refused),
  checks its length against the manifest, and streams it through the same
  `write_into_slot` as the card install (which clears the slot's record
  before the erase, ticket 08).
- **C API** (`thistle/update.h`): `UPDATE_WAITING`, `rs_update_offer`,
  `rs_update_last_check`, `rs_update_check_now`, `rs_update_install(bool
  mobile_data_allowed)`, `rs_update_needs_consent`.
- **Settings → System update**: the last check and its time, Check now, the
  release on offer and its notes, Download and install, Download over
  mobile data, progress, then Restart now / Later; Go back and the card's
  rows below.
- **The App Store's Firmware tab and its UPDATE_OS job are gone**; the
  Installed tab stays at 197,38.
- **Log**: `update check (the daily check|Check now|the App Store's
  Refresh): 0.6.0 is offered; running ...` / `up to date; running ..., the
  newest for this board is ...`, `update check (...) failed: ...`,
  `downloading 0.6.0: <url>`, `the download waits: ...`, `installed ...`,
  `update refused: <reason>`; keeping and rolling back are ota.rs's lines.
- **Simulator**: the daily check runs only with
  `THISTLE_SIM_UPDATE_CHECKS=1` (the simulator's card names the real
  catalog, which every other test would fetch); `THISTLE_SIM_NETWORK=mobile`
  takes the host network down, so with mobile data on the fake modem is the
  network; `THISTLE_SIM_CA_BUNDLE` lets the test's catalog server speak
  HTTPS for the HTTPS-to-HTTP case.
- **Tests**: `cargo test` 2078 passed; `test_system_update.py` 23 of 23
  (7 new: the daily check's offer, kept catalog and record, and no second
  check that day; nothing offered older or for another board or chip;
  Check now, download through a 302, Later, the next run booting and
  keeping 0.6.0, then up to date; a size mismatch; HTTPS to HTTP; six
  redirects; mobile data waiting, then allowed); `test_kernel_slots.py`,
  `test_appstore.py`, `test_tap_appstore.py`, `test_epaper_partial_flush.py`
  pass. `idf.py build` succeeds.

**Left for the user:** the T-Deck Max box. Flash a laptop build of `ota`,
point the card's `config/appstore.json` at a catalog listing a release
candidate newer than the laptop build's tag (the fork's Pages catalog lists
no prereleases, so a hand-written catalog naming the rc's release assets),
then Settings → System update → Check now, Download and install, and
compare the boot log's `internal RAM N KB free` line before and after a
check. A real GitHub download goes through `release-assets.githubusercontent.com`.
