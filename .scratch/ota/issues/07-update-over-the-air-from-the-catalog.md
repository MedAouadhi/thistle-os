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

- [ ] `update_policy` decides, tested on the host: which release to offer
      (newer, equal, older, a laptop build's base, wrong chip or board),
      whether a check is due (clock set and unset), and whether a download
      may start (WiFi, mobile data with and without consent).
- [ ] A kernel worker job notices a check is due; the check and any download
      run on one FreeRTOS task made for them and gone afterwards, with an
      internal stack that never holds the catalog's entries.
- [ ] The C API gives the state (idle, checking, up to date, available,
      downloading, writing, ready to restart, failed with a reason), the
      version on offer and its notes, and calls for check now, install
      (with mobile-data consent) and restart.
- [ ] The simulator test runs against a fake GitHub (a catalog server, and an
      assets server behind 302s): the offer after a check, install, Restart
      later and the next run booting the new version, a size mismatch, a
      redirect to HTTP, six redirects, and mobile data waiting for consent.
- [ ] The log says what each check found, what was offered, installed, kept
      or rolled back, and why anything was refused.
- [ ] On the T-Deck Max, with `config/appstore.json` pointing at a catalog
      that lists a release candidate, the device offers it, downloads it
      from GitHub and installs it; the boot log's internal RAM line is the
      same before and after a check.
