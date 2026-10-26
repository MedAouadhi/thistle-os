# Releases and over-the-air updates

Status: ready-for-agent

Decisions: ADR-0010 (the kernel updates itself into two kernel slots),
ADR-0011 (releases on GitHub, catalog on Pages). Vocabulary: `CONTEXT.md`,
"Releases and updates": publisher, release, catalog, artifact manifest,
kernel slot, security version, security floor.

## Problem Statement

The user develops ThistleOS on their own fork and wants their devices to
update over the air from a source they trust, without hosting a website.
None of that works today:

- The T-Deck Max cannot update its kernel over the air. The App Store's
  "Update OS" writes ESP-IDF's next update partition, which from `factory`
  is Recovery's 2.5 MB slot, and the kernel no longer fits in it.
- Nothing publishes a firmware a device could find. The catalog has no
  firmware entries, and the releases that exist are upstream's.
- The kernel checks only a signature over the image, and only when the
  catalog names one. It checks no version, chip, board or security floor.
  Recovery checks all of those, but has never run on a T-Deck Max.
- The pipeline is upstream's. It runs on wan0net's self-hosted runners,
  signs with their key and publishes the catalog to their repository.
  Devices trust wan0net's key, even ones built on the user's laptop.
- A build compiles `.env`'s PodcastIndex and Cohere keys and the Hermes
  bridge's address in, so publishing a firmware would publish them.
- Nothing tells the user that an installed app has a newer release.
- A release that changes a board profile never reaches a board that already
  has one, as tickets 03, 06, 09 and 10 of the T-Deck Max found.

## Solution

**Publishing.** The user releases by pushing a tag. `v0.6.0` releases the
firmware and `notes-v1.0.3` releases Notes. GitHub Actions, on GitHub's
free hosted runners, builds that commit and signs it with the user's one
release key. It publishes an immutable GitHub Release with build provenance
attestations, then rebuilds the catalog on the fork's GitHub Pages from the
releases. There is no server, and nothing in the catalog has to be kept by
hand.

**On the device.** Once a day, the device reads the catalog over whatever
network it has.

- Home shows a row when ThistleOS or an app has an update.
- Settings → System update shows the new version and its notes. It
  downloads over WiFi, or over mobile data if the user allows it, and
  checks the download against the publisher's signed artifact manifest.
  It then writes the new kernel into the other kernel slot and asks
  Restart now or Later.
- A new kernel that does not run for a minute is rolled back at the next
  reset. Settings offers to go back to the previous version, which is
  still in the other kernel slot.
- The App Store's Installed tab offers app updates.
- A release copied to the card installs with no network, through the same
  checks.

**Moving a device over.** One USB flash of a laptop build moves a device to
the new partition table, the user's key and the user's catalog. The flash
keeps its settings, WiFi and mesh identity.

## User Stories

### Publishing

1. As the publisher, I want to release the firmware by pushing a tag such as
   `v0.6.0`, so that a release is one command.
2. As the publisher, I want to release one app by pushing a tag such as
   `notes-v1.0.3`, so that fixing an app needs no firmware release.
3. As the publisher, I want releases built on GitHub's free hosted runners,
   so that I run and pay for no machine.
4. As the publisher, I want a release built from the tagged commit and
   nothing else, so that what devices install is exactly what the tag names.
5. As the publisher, I want the release key to exist only as a secret that
   release tags alone can read, so that a workflow on a branch cannot sign
   anything.
6. As the publisher, I want only myself able to create release tags, so that
   nobody else can start a release.
7. As the publisher, I want one release key to sign the firmware and the
   apps, so that I have one secret to look after.
8. As the publisher, I want released firmware to compile in nothing from
   `.env`, so that publishing a firmware never publishes my Cohere or
   PodcastIndex keys or my tailnet's addresses.
9. As the publisher, I want a firmware release's notes taken from
   CHANGELOG.md's section for its tag, so that I write them in one place.
10. As the publisher, I want an app release's notes taken from the app's own
    metadata for that version, so that each app keeps its own history.
11. As the publisher, I want published releases to be immutable, so that a
    device never finds different bytes behind the same version.
12. As the publisher, I want each release to carry build provenance
    attestations, so that anyone can check an artifact came from my
    workflow at that commit.
13. As the publisher, I want a tag with a suffix, such as `v0.6.0-rc.1`, to
    publish a prerelease that no device is offered, so that I can try it by
    USB first.
14. As the publisher, I want an app tag refused when its version differs from
    the app's package manifest, so that the tag and the package never
    disagree.
15. As the publisher, I want an app tag refused when its `min_os` is newer than
    my latest firmware release, so that I never publish an app no device can
    install.
16. As the publisher, I want the catalog rebuilt from my releases on every
    Pages deploy, so that it never drifts from what is released.
17. As the publisher, I want a published release to redeploy Pages, so that
    devices see it at their next check.
18. As the publisher, I want firmware built for the ESP32-S3 only, so that I
    publish nothing I cannot test, and I want adding a chip to be one line.
19. As the publisher, I want a firmware release to include a full flash image
    (bootloader, partition table and kernel), so that a new device can be
    set up by USB from a release.
20. As the publisher, I want to raise the security version by hand in a
    release that fixes a hole, so that no device that has run it can be
    taken back below it.
21. As the publisher, I want CI on every push and pull request to build the
    firmware and run the tests on hosted runners, so that a release tag
    lands on a commit already known to build.
22. As the publisher, I want upstream's workflows replaced, so that no job
    waits forever for wan0net's runners.
23. As the publisher, I want every wan0net link in the docs, the site, the
    README and the apps' metadata pointed at my fork, so that readers land
    on what they are using.

### Updating the firmware

24. As the device owner, I want my device to read the catalog once a day on
    any network, so that I learn of updates without asking.
25. As the device owner, I want a "Check now" in Settings → System update, so
    that I can look straight after publishing.
26. As the device owner, I want Home to show a row when ThistleOS or my apps
    have updates, naming them, so that I notice without opening Settings.
27. As the device owner, I want Settings → System update to show the version
    I run, the version on offer and its notes, so that I know what I am
    installing.
28. As the device owner, I want to be offered only a release newer than the
    one I run, so that the device never suggests a step back.
29. As the device owner, I want the download to wait for WiFi unless I allow
    mobile data, so that an update does not use up my data plan unasked.
30. As the device owner, I want progress shown while the update downloads
    and writes, and the device held awake meanwhile, so that sleep never
    cuts it short.
31. As the device owner, I want a firmware refused, with the reason shown,
    when my publisher did not sign it, or it is for another chip or board,
    or it is below my device's security floor, so that only what I
    released, for this board, gets installed.
32. As the device owner, I want a download refused when its size or hash
    differs from the signed artifact manifest, so that a corrupted or
    swapped file never reaches flash.
33. As the device owner, I want the running kernel left untouched by a
    failed download or check, so that a failure costs nothing but the
    attempt.
34. As the device owner, I want to choose Restart now or Later once the
    update is written, Later meaning my next restart of any kind, so that
    an update never interrupts me.
35. As the device owner, I want a new kernel that does not run for a minute
    rolled back at the next reset, so that a broken release undoes itself.
36. As the device owner, I want to go back to the previous version from
    Settings, with no download, while it is at or above the security floor,
    so that I can step away from a release that boots but misbehaves.
37. As the device owner, I want never to be able to install or go back to a
    firmware below the security floor, from the network or the card, so
    that a fixed hole cannot be reopened.
38. As the device owner, I want to install a release copied to the card with
    no network, through the same checks, so that I can update without a
    connection.
39. As the device owner, I want the first boot of a different firmware
    version to rewrite the stored board profile from its embedded copy and
    keep the old one beside it, so that a release's profile changes reach
    my board.
40. As the device owner, I want going back to bring back that version's board
    profile too, so that a kernel never runs with a profile from a newer
    one.
41. As the device owner, I want my settings, WiFi, mesh identity and card to
    survive every update, so that updating costs me nothing.
42. As the device owner, I want downloads to follow GitHub's redirects, never
    from HTTPS to HTTP and at most five, so that release assets download
    without any address being trusted for its own sake.
43. As the device owner, I want the log to say what each check found, what
    was offered, installed, kept or rolled back, and why anything was
    refused, so that a failed update can be diagnosed from a capture.
44. As the device owner on released firmware, I want Dictation, Podcasts,
    Hermes and AssistNow to read their keys from my card and to say what
    they lack, so that they work without keys compiled in.

### Updating apps

45. As the device owner, I want the App Store's Installed tab to mark apps
    that have a newer release and update one on a tap, so that my apps stay
    current.
46. As the device owner, I want app releases that need newer firmware than
    mine hidden, so that I am never offered an app that will not load.
47. As the device owner, I want the daily check and the App Store's Refresh to
    keep one copy of the catalog, so that they never disagree.
48. As the device owner, I want the App Store's Firmware tab gone, so that
    system updates have one place.

### Developing

49. As a developer, I want `idf.py flash` to keep working, writing the first
    kernel slot and resetting the boot choice, so that USB flashing is as
    before.
50. As a developer, I want a laptop build to report the version it was built
    from (`git describe`) and to count as that tag, so that it is offered
    only a release newer than its base.
51. As a developer, I want laptop firmware to trust the release key and my
    laptop's publisher key for apps, so that I can sideload apps I build.
52. As a developer, I want released firmware to trust the release key and no
    other, so that a copied laptop key can sign nothing for it.
53. As a developer, I want one USB flash to move a device to the new
    partition table, my key and my catalog, keeping NVS and SPIFFS, so that
    moving over costs one flash.
54. As a developer, I want the simulator to run the whole update flow against
    a fake GitHub, so that I can change it without hardware.
55. As a developer, I want every update decision tested on the host, so that
    the edge cases are cheap to cover.

## Implementation Decisions

### The partition table

Both kernel slots and the reserved slot replace `factory`, `ota_0` and
`ota_1`. Everything else stays at the same offset and size, so NVS, SPIFFS
and the core dump survive the migration:

| Name     | Type / subtype | Offset   | Size     | Holds                    |
|----------|----------------|----------|----------|--------------------------|
| nvs      | data / nvs     | 0x9000   | 0x6000   | unchanged                |
| otadata  | data / ota     | 0xF000   | 0x2000   | unchanged                |
| phy_init | data / phy     | 0x11000  | 0x1000   | unchanged                |
| kernel_a | app / ota_0    | 0x20000  | 0x480000 | a kernel                 |
| kernel_b | app / ota_1    | 0x4A0000 | 0x480000 | a kernel                 |
| recovery | app / ota_2    | 0x920000 | 0x280000 | reserved, nothing writes |
| storage  | data / spiffs  | 0xBA0000 | 0x450000 | unchanged                |
| coredump | data / coredump| 0xFF0000 | 0x10000  | unchanged                |

- There is no `factory` partition, so `idf.py flash` writes `kernel_a` and
  the initial otadata.
- The kernel always names the other kernel slot by its label. It never asks
  ESP-IDF for the next update partition, which from `kernel_b` is the
  reserved slot.
- ESP-IDF warns that the kernel is larger than the smallest app partition.
  That warning was already there, and the build goes on.

### Versions

- **The firmware's version is its tag**, taken by `git describe` into
  ESP-IDF's project version. The kernel reports it everywhere it now
  reports the fixed `0.5.0`: the version module that `min_os` is checked
  against, the OTA module, Settings and the boot log.
- **A laptop build** reads as, for example, `0.6.0-3-gabc123-dirty`. It
  counts as `0.6.0` when releases are compared and when `min_os` is checked.
- **The security version stays a number in the source**, raised by hand.
  The release workflow writes it into the artifact manifest.
- **The security floor is the NVS value Recovery already uses**
  (`thistle_sec` / `fw_sec_ver`). It rises when a new kernel is kept, not
  when it is written, so a kernel that is rolled back never raises it.
- **An app's release sequence is computed from its version**: major × 10⁶ +
  minor × 10³ + patch. The release workflow writes it into the package, and
  the package manifest's `version` must equal the tag's.

### The artifact manifest

- **The kernel takes Recovery's canonical V1 artifact manifest unchanged**:
  type, id, version, security version, arch, compatible boards,
  destination, sha256, size, url.
- **Its parsing and device checks move into a shared crate** next to
  thistle-tk. The kernel uses it; Recovery can switch to it when it is
  brought up.
- **A firmware manifest's destination names the kernel**, not a partition.
- **The kernel downloads from the catalog's URL.** The manifest's signed
  size and hash bind the bytes, so its URL is never fetched.
- **A firmware is accepted only if all of these hold:**
  - the manifest's signature verifies against the kernel's key;
  - its type is firmware;
  - its arch is the running chip's;
  - the stored board id is among its compatible boards;
  - its security version is at least the floor.

  An online offer also needs a version newer than the running one. A card
  install may be any version at or above the floor.

### Keys

- **The kernel's key becomes the user's release key** in place of
  upstream's production key. Every build is a cargo release build, so
  laptop builds embed it too.
- **The laptop's publisher key stays a build-time addition** that only a
  laptop build has. CI never sets it.
- **Firmware, drivers and window managers accept the kernel's key only**,
  as now.
- **The release key** is generated once and stored only as a secret of a
  GitHub `release` environment whose deployment rule admits only tags
  matching `v*` and `*-v*`. A tag ruleset restricts who may create those
  tags. The tracked public key file is updated to match.

### The release workflows

All of them run on `ubuntu-latest`. The jobs that build firmware or apps
run in the `espressif/idf:v5.5` container; the jobs that check a tag, sign,
publish and deploy Pages need no ESP-IDF and run on the bare runner.
None creates `.env`, and the release key is passed only to the step that
signs.

- **CI**, on every push and pull request: the S3 firmware build, the
  partition size check, and the host tests that `tests.yml` runs. It
  replaces upstream's `build.yml`. `tests.yml` and `pages.yml` move to
  hosted runners.
- **Firmware release**, on a tag matching `v*`:
  1. Builds the S3 firmware and checks its size against a kernel slot.
  2. Writes and signs the artifact manifest.
  3. Writes the release's catalog entry.
  4. Merges a full flash image.
  5. Creates a draft release with the image, the manifest, its signature,
     the full flash image, the catalog entry and `SHA256SUMS`.
  6. Attests their provenance, publishes the release, and calls the Pages
     workflow.
  7. A suffixed tag publishes a prerelease instead.
  8. CHANGELOG.md's section for the tag becomes the notes.
- **App release**, on a tag matching `<app>-v*`, where `<app>` is a
  directory among the four App Store apps:
  1. Checks the tag's version against the package manifest, and `min_os`
     against the latest firmware release.
  2. Builds the app for the S3, signs its ELF, builds and signs its TAP, and
     writes its catalog entry.
  3. Then creates the draft, attests, publishes and calls Pages, as the
     firmware release does.
- **Pages workflow**, on a push to main or when a release workflow calls it:
  1. Builds the docs and the WASM simulator, as now.
  2. Lists the repository's releases through the API and skips drafts and
     prereleases.
  3. Takes the newest firmware release by version and the newest release of
     each app.
  4. Collects each one's catalog entry into `store/catalog.json`.
  5. A release publishes through the workflow's own token, which starts no
     other workflow, so the release workflows call Pages as a reusable
     workflow rather than waiting for a `release` event.
- **Upstream's `release.yml` and `apps.yml` are removed**, and so are their
  self-hosted runner labels and the deploy to `wan0net/thistle-apps`.
- **Set up once by hand**, outside the workflows:
  - the `release` environment and its secret;
  - the tag ruleset;
  - immutable releases;
  - Pages served from GitHub Actions;
  - the `github-pages` environment's deployment rule admitting tags
    matching `v*` and `*-v*` beside `main`: a release workflow calls Pages
    from its tag's run, and by default that environment admits only the
    default branch, so the deploy is refused.

### The catalog

- **The catalog builder takes a Releases API listing.** Its output is the
  catalog the device already parses: `{"version": 1, "generated", "entries"}`.
- **An app entry keeps the fields the builder writes today**, with
  `min_os_version` and `changelog` filled in.
- **A firmware entry is flat**, because the device's parser ends an entry at
  its first closing brace. Its fields: `type: "firmware"`, `id`, `version`,
  `arch`, `compatible_boards`, `url`, `manifest_url`, `manifest_sig_url`,
  `sha256`, `size_bytes`, `changelog`, `updated`.
- **The catalog is unsigned.**
- **The default catalog URL** becomes
  `https://medaouadhi.github.io/thistle-os/store/catalog.json`. A card's
  `config/appstore.json` still overrides it.

### Downloads

- **Every download follows redirects**: the catalog, the manifest, its
  signature, the firmware, a package and its signature.
- **At most five**, and **never from HTTPS to HTTP**. A plain-HTTP start, as
  the simulator's fakes use, may redirect within HTTP.
- **What is installed is trusted for its signature and its hash, never for
  its host.**

### The update manager

A new kernel module pair, following the policy and manager split of
`power_policy`/`power_manager` and `time_policy`/`time_manager`.

**`update_policy` is pure and host-tested.** Its decisions:

- **Offer.** Given the running version, chip, board id, floor and the
  catalog's firmware entries, which release to offer, if any. Given the
  installed apps' release sequences and the running version, which app
  releases to offer.
- **Accept.** Given a parsed artifact manifest and the device, whether to
  accept it, and the reason if not.
- **Going back.** Given the other kernel slot's record and image description
  and the floor, whether to offer going back, and to what.
- **Board profile.** Given the stored profile's writer version and the
  running version, whether to rewrite it.
- **When to check.** Given the last check time and whether the clock is set,
  whether a check is due. With the clock unset, it checks at most once a
  boot.
- **Mobile data.** Given the network and the user's mobile-data consent,
  whether a download may start.
- **Versions.** Reading a `git describe` version, and comparing versions.

**`update_manager` runs them.**

- **The daily check.** A kernel worker job notices a check is due once a
  network is up. The check and any download then run on one FreeRTOS task
  made for the job and gone after it, as the tailnet's start does, never on
  the worker or a `std::thread`. The stack is internal, sized like the App
  Store worker's, and the catalog's entries are never held on it. It stores
  the catalog where the App Store reads it, and records the time of the
  check.
- **An install.**
  1. Holds the device awake.
  2. Fetches and checks the manifest and its signature.
  3. Erases the other kernel slot and streams the firmware into it, hashing
     as it writes.
  4. Checks size and hash, lets ESP-IDF validate the image, and records the
     slot's version and security version.
  5. Selects the slot.

  It reports progress and the outcome through the state below. A failure at
  any step leaves the boot choice as it was. Once the erase starts, the
  previous kernel is gone, and going back is no longer offered.
- **Each kernel slot's version and security version are recorded in NVS**
  when the slot is written, and by a kernel for its own slot when it runs.
  Going back is offered only while the record matches the slot's own image
  description.
- **Keeping a new kernel.** A kernel whose image is pending verification
  marks itself valid after 60 s of running, raises the floor to its own
  security version, and finishes the bundle transaction. The power manager
  starts after that. On every other boot nothing waits.
- **Going back** selects the other kernel slot and restarts.
- **The card install** reads a `.bin`, its manifest and the manifest's
  signature from `/sdcard/update/` and takes the same path. It replaces
  today's SD apply.
- **The board profile.** The stored profile records the version that wrote
  it. At boot, if that version is not the running one and the kernel embeds
  a profile for the stored board id, the kernel rewrites the profile and
  keeps the previous one as a single backup. A board id the kernel has no
  profile for keeps its profile.

**The C API (`thistle/update.h`, `rs_update_*`)** gives Settings, Home and
the App Store:

- the state: idle, checking, up to date, available, downloading, writing,
  ready to restart, or failed with a reason;
- the version on offer and its notes;
- the number of app updates;
- the version going back would reach;
- calls: check now, install (with mobile-data consent), install from card,
  restart, go back.

### Device UI

- **Settings → System update** shows:
  - the running version, and when and what the last check found;
  - Check now;
  - the release on offer, its notes, and Download and install;
  - a mobile-data confirmation when the device is on mobile data;
  - progress;
  - Restart now / Later;
  - Go back to `<version>`;
  - Install from card, when the card holds a release.
- **Home's "what is new" rows** gain one: ThistleOS's version on offer
  and/or the number of app updates. It reads kernel state once a second,
  as the other rows do, and opens Settings → System update, or the App
  Store when only apps have updates.
- **The App Store** loses its Firmware tab. Its Installed tab marks apps
  with an update and gains an Update button, which installs through the
  existing TAP path. The TAP installer also checks the package's `min_os`.

### No keys in released firmware

- The credentials steps already fall back to empty values when `.env` is
  absent, and each feature already reads a card override: Podcasts,
  Dictation, AssistNow and Hermes.
- Each of those features says what it lacks when it has no key.
- Nothing else changes on the device.

### Simulator

- **Kernel slots are two files in a directory** (`THISTLE_SIM_SLOTS`), with
  the boot choice and pending state in a third.
- **The running version** is the one recorded for the selected slot, if any,
  else the build's. So a restart "boots" a new version without the binary
  changing.
- **A run that ends before its new kernel is kept** leaves it pending, and
  the next run boots the other slot, as the bootloader would.
- **The keep delay is shortened** by an environment variable.
- **A test publisher key**, named by an environment variable, is trusted in
  simulator builds only, so tests can sign artifact manifests.

## Testing Decisions

A good test here drives a module through its public surface and checks what
a user or a device would see: what is offered, what is refused and why,
which slot boots, and what the catalog lists. It never checks how the module
got there.

- **`update_policy`** is tested with host `cargo test`, as `power_policy`,
  `time_policy` and `tailnet_policy` are. Every rule in the Implementation
  Decisions gets a case:
  - versions newer, equal and older; laptop versions; malformed versions;
  - wrong chip; wrong board; below, at and above the floor;
  - a bad signature reported as refused;
  - going back to a slot whose record does not match its image;
  - the board profile's writer version equal, different or missing;
  - a check due with the clock set and unset;
  - mobile data with and without consent;
  - app releases filtered by `min_os` and release sequence.
- **The shared artifact manifest crate** carries the canonical-format tests
  that Recovery's module has today. It is the first prior art to move
  across.
- **`simulator/tests/test_system_update.py`** runs the simulator against a
  fake GitHub, as `test_gps_assist.py` runs against a fake Thingstream and
  `test_appstore.py` against a local catalog host. One server serves the
  catalog, and a second "release assets" server sits behind 302 redirects.
  Its cases:
  - the daily check and the Home row;
  - install, Restart later, and the next run booting the new version;
  - a run ending before the keep delay, rolled back;
  - going back;
  - a card install;
  - refusals for bad signature, wrong board, below floor, size mismatch,
    redirect to HTTP, and six redirects;
  - mobile data waiting for consent;
  - an app update from the Installed tab;
  - the board profile rewritten on a version change.

  Each case runs on a scratch card, SPIFFS and slot directory.
- **The catalog builder** is tested over a recorded Releases API listing with
  their catalog entries, as `tools/test_build_catalog.py` tests the builder
  today. Its cases:
  - newest firmware chosen by version, not by date;
  - the newest release of each app;
  - drafts and prereleases left out;
  - flat firmware entries;
  - the release workflow's own checks: tag against version, and `min_os`
    against the latest firmware.
- **The workflows** are proven once by a `v0.6.0-rc.1` tag on the fork,
  which builds, signs and publishes a prerelease without any device seeing
  it, then by `v0.6.0` and one app tag.
- **The T-Deck Max hardware pass** is a checklist in the last ticket:
  - the USB migration keeping NVS, SPIFFS, WiFi and the mesh identity;
  - a real download through GitHub's redirect;
  - a write into the other slot;
  - Restart later;
  - a deliberately crashing kernel rolled back within its first minute;
  - going back;
  - a card install;
  - the power manager starting after the keep;
  - internal RAM before and after a check, from the boot log's line.

## Out of Scope

- Bringing Recovery up on the T-Deck Max. Its slot is reserved, and its
  catalog URLs, keys and partition table stay as they are until then.
- Secure boot, flash encryption and eFuse anti-rollback.
- Chips other than the ESP32-S3.
- Releasing drivers, window managers or board profiles. The standalone
  `kbd_tca8418` driver and the `hello` and `terminal` sample apps stay out.
- Signing the catalog, a test channel, automatic installs, and delta
  updates.
- An offline backup of the release key.
- An updater for thistle-tk. Its App Store is its own, and Home's row is
  LVGL's. The kernel's daily check still runs under it.

## Further Notes

- **Tickets:** `issues/01` to `issues/10`, each listing what blocks it.
  01, 02 and 03 can start at once. Ticket 05 is the first end-to-end path,
  from a tag to the device installing an app. Ticket 03 needs the one USB
  flash.
- **Changed while ticketing:** an app tag's `min_os` is checked against the
  newest firmware release *including prereleases*, so an app can follow a
  release candidate. Devices still hide whatever they cannot run.
- **The GitHub settings set up by hand** (environment, secret, ruleset,
  immutable releases, Pages source) suit a walkthrough script.
- **On sizes:** a kernel slot is 4.5 MB and the kernel is about 2.6 MB. The
  CI size check moves from `ota_1` to the kernel slots.
- **Internal RAM:** the check's task exists only while it runs. Its stack
  comes out of the 150 KB of internal RAM the boot log reports, so measure
  it there.
- **The first release**, `v0.6.0`, has to be on a device already moved by
  USB. Until then, laptop builds of the fork's commits report `0.5.2-N-g…`
  and count as `0.5.2`.
