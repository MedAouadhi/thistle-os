# 04: Release the firmware by tag

**What to build:** Pushing a tag such as `v0.6.0-rc.1` to the fork builds the
S3 firmware from that commit, signs it with the user's release key, and
publishes a GitHub Release, with nothing else to do by hand. The firmware's
version is its tag: the kernel reports what `git describe` gives at build time
wherever it now reports the fixed `0.5.0`, and a laptop build such as
`0.5.2-108-gabc123-dirty` counts as `0.5.2`. The user's own release key
replaces upstream's production key in the kernel. Spec: `../spec.md`,
"Versions", "The artifact manifest", "Keys" and "The release workflows".
ADR-0011.

The one-time GitHub setup is part of this ticket and needs the user's
go-ahead, since it creates a key and changes repository settings: an Ed25519
release key generated straight into the secret of a `release` environment
whose deployment rule admits only tags matching `v*` and `*-v*` (never
written to disk or a log); a tag ruleset so only the owner creates those
tags; immutable releases turned on.

**Blocked by:** 02 (CI on GitHub-hosted runners)

**Status:** ready-for-agent

- [x] Version reading and comparison are tested on the host: plain tags,
      `git describe` forms, `-dirty`, release candidates (`0.6.0-rc.1` is
      below `0.6.0`, and `0.6.0-rc.1-3-gabc` counts as `0.6.0-rc.1`), and
      malformed input.
- [x] `git describe` matches firmware tags only (`v[0-9]*`), so an app tag
      such as `notes-v1.0.1` never becomes the firmware's version.
- [x] The boot log, Settings and `min_os` checks use the version; a laptop
      build shows its describe string.
- [ ] The kernel's key and the tracked public key file are the release key;
      every driver the T-Deck Max loads still loads, and laptop-signed apps
      still install on a laptop build.
- [ ] `v0.6.0-rc.1` publishes a prerelease carrying the image, its artifact
      manifest (canonical V1, destination naming the kernel, the security
      version from the source) and signature, the release's flat catalog
      entry, a full flash image (bootloader, partition table, kernel) and
      `SHA256SUMS`, with CHANGELOG.md's section as its notes.
- [ ] The release is created as a draft and published only after every asset
      is attached; once published, an upload to it is refused.
- [ ] `gh attestation verify` passes for the image, and the manifest's
      signature verifies against the committed public key with the repo's
      signing tool.
- [ ] A tag without a suffix publishes a full release; only the signing step
      receives the key.

## Comments

**2026-10-26, implementer.** Done on branch
`worktree-agent-a914f2db22ea890d0`, with `ota`'s tip (tickets 01 and 03)
merged in. Nothing pushed, no tag created, no GitHub setting changed.

**The one-time GitHub setup is done**, by the user, on
MedAouadhi/thistle-os, and checked read-only here: the `release`
environment admits tags `v*` and `*-v*` and holds the secret
`RELEASE_SIGNING_KEY` (64 hex digits of the Ed25519 seed); the ruleset
"release tags" is active; immutable releases are on; Pages is served from
GitHub Actions. The key's public half is
`ac8797a91177f7946a1330a14f9d484cb7fd024d08c13d208a04846a88dbdd3e`.

What was built:

- **The version.** `tools/firmware_version.sh` runs `git describe --tags
  --match 'v[0-9]*' --dirty` and drops the `v` (`0.0.0-unknown` with no
  tag in reach). The top-level `CMakeLists.txt` makes it ESP-IDF's
  `PROJECT_VER` (cut to the 31 characters `esp_app_desc_t` holds, and
  reconfigured when HEAD moves, as ESP-IDF's own describe is);
  `kernel_rs/CMakeLists.txt` passes it to cargo as `THISTLE_FW_VERSION`, so
  the kernel reports exactly what the image description holds. The
  simulator and host tests get it from the same script in `build.rs`.
  `version.rs` has `Version::parse` (tags and describe strings; a hyphen in
  a prerelease is refused, as it would read as describe's), semver
  ordering, `meets(running, min_os)`, `running()`. `ota_get_current_version()`
  (the simulator's slot version first, ticket 03), `kernel_version()`, the
  Terminal, Settings' About (buffer widened to 64) and the boot log's first
  line report it; `THISTLE_VERSION_*` and the fixed `0.5.0`/`0.1.0` are
  gone. A malformed `min_os` is now refused, where it read as 0.0.0.
- **The key.** `kernel_boot.rs`'s `RELEASE_KEY` and
  `tools/production_public.key` are the release key; a host test checks
  them against each other and against the hex above. The debug-only dev
  key branch is gone (every build is a cargo release build). Recovery's
  keys are untouched (out of scope).
- **The release.** `.github/workflows/release-firmware.yml`, on `v*`:
  `build` (no environment, no secret; checkout with `fetch-depth: 0`; the
  `.env` guard; fails unless both `firmware_version.sh` and
  `project_description.json` say the tag; size check; `esptool merge_bin
  @flash_args` for the full image: bootloader, partition table, initial
  otadata, kernel) then `release` (environment `release`): `tag`, `stage`,
  **Sign the artifact manifest** (the only step with the secret), a check
  of the signature with `tools/sign.py verify --pubkey
  tools/production_public.key`, `SHA256SUMS`, `gh release create --draft
  --verify-tag --notes-file` (`--prerelease` for a suffixed tag),
  `actions/attest@v4` over `dist/*`, `gh release edit --draft=false`; then
  calls `pages.yml`, which gained `workflow_call`. The steps are
  `tools/release_firmware.py` (tag, stage, sign, sums). Assets:
  `thistle_os-esp32s3-<tag>.bin`, `.bin.manifest`, `.bin.manifest.sig`,
  `-full.bin`, `.catalog-entry.json` (the flat entry; ticket 05's builder
  can collect every `*.catalog-entry.json`), `SHA256SUMS`. A firmware
  manifest's destination is now `kernel` (`tools/sign.py`). A tag with no
  `## <tag>` section in CHANGELOG.md fails before anything is created.
- CI's firmware job, `tests.yml` and `pages.yml` check out the tags too, so
  their builds have a version; `tests.yml` runs the two new tool tests.

Tests: `cargo test` 2018 passed; every `tools/test_*.py` passes (new:
`test_firmware_version.py`, a scratch repository with app and sprint
tags; `test_release_firmware.py`; workflow rules in
`test_ci_workflows.py`); actionlint 1.7.7 (without shellcheck) finds
nothing. `idf.py build` reports `ThistleOS version: 0.5.2-187-g5eb7e41`,
the string is in the image, and a commit reconfigures. The simulator
builds, logs `running kernel_a, version 0.5.2-187-g5eb7e41`, and passes its
boot test, `test_kernel_slots.py` and `test_appstore.py`. A local dry run
of stage/sign/sums on that build as `v0.6.0-rc.1`, with a throwaway key,
wrote the six assets and a 3.8 MB image; the committed key refuses its
signature, as it should.

**Left, needing the user:**

- **Boxes 5 to 8 need the real `v0.6.0-rc.1` tag**, then `v0.6.0`, pushed
  to the fork after the user approves. The tag's commit needs a
  `## v0.6.0-rc.1 - <date>` section in CHANGELOG.md, or the release stops
  at "Stage the release". Then: `gh attestation verify
  thistle_os-esp32s3-v0.6.0-rc.1.bin --repo MedAouadhi/thistle-os`;
  `python3 tools/sign.py verify <manifest> --pubkey
  tools/production_public.key`; `gh release upload` to the published
  release should be refused.
- **The `github-pages` environment admits only `main`**, so the Pages call
  from a tag run will be refused at its deploy job until tag rules `v*`
  and `*-v*` are added to that environment (Settings, Environments,
  github-pages). The release itself is published before that job.
- Box 4: the key part is done. The T-Deck Max's drivers in its profile are
  compiled in (no `.drv.elf` in the firmware), so nothing it loads by
  default checks a signature; a `.drv.elf` left on SPIFFS or the card
  signed by upstream's key would now be refused. Laptop-signed apps take
  the same path as before (`THISTLE_APP_PUBLISHER_KEY`). Both want the
  hardware pass (ticket 10).
- Ticket 06's shared manifest crate must accept a prerelease version
  (`0.6.0-rc.1`) and the destination `kernel`; Recovery's parser accepts
  neither, and stays as it is.
- The simulator's cargo step runs only when kernel sources change, so its
  version can lag a commit until then; the firmware's never does.
