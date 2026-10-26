# 05: Release an app by tag and install it from your catalog

**What to build:** The first end-to-end path. Pushing a tag such as
`notes-v1.0.1` builds that App Store app for the S3, signs its ELF and TAP with
the release key, and publishes a GitHub Release; Pages then rebuilds the
catalog from the repository's releases; and the device's App Store, reading
that catalog by default, installs the app through GitHub's redirect. The
catalog is derived and holds nothing of its own: each release carries its own
catalog entry, and the builder takes the newest full release of each
component by version. Spec: `../spec.md`, "The release workflows" and "The
catalog". ADR-0011.

**Blocked by:** 01 (Downloads follow redirects), 04 (Release the firmware by tag)

**Status:** ready-for-agent

- [x] An app tag is refused when its app is not one of the four App Store
      apps, when its version differs from the package manifest's, or when its
      `min_os` is newer than the newest firmware release, prereleases
      included (a change from the spec, agreed: an app may need a release
      candidate's kernel, and devices hide what they cannot run).
- [x] The release sequence is computed from the version (major x 10^6 +
      minor x 10^3 + patch) and written into the package; the notes come from
      the app's metadata for that version.
- [x] App and firmware releases both call the Pages workflow when they
      publish, since a release made with the workflow's token starts no other
      workflow.
- [ ] The catalog builder, tested over a recorded Releases API listing,
      takes the newest firmware by version (not date) and the newest release
      of each app, leaves out drafts and prereleases, and writes flat
      firmware entries; its output is served at `store/catalog.json` on the
      fork's Pages, and a docs-only deploy rebuilds the same catalog.
- [x] The device's default catalog URL is the fork's Pages catalog, a card's
      `config/appstore.json` still overrides it, and the simulator's card
      points at it too.
- [ ] On the T-Deck Max running a laptop build, the App Store's Refresh lists
      the released app from the fork's Pages and installs it.

## Comments

**2026-10-26, implementer.** Done on branch
`worktree-agent-a83f713ba35c5a3a5`, with `ota`'s tip merged in. Nothing
pushed, no tag created, no GitHub setting changed.

What was built:

- **`tools/release_app.py`**, the app release's steps, as
  `release_firmware.py` is the firmware's: `check` (the tag is one of the
  four App Store apps, `<app>-vX.Y.Z[-suffix]`; its version is
  `manifest.json`'s; `min_os` is no newer than the newest firmware release
  by version, prereleases included, drafts not; no firmware release at all
  is refused too), `stage` (the release sequence, major x 10^6 + minor x
  10^3 + patch, written into the manifest and `metadata.json`'s release;
  the notes are that release's `changes`, which must be the metadata's
  newest), `sign` (the ELF signed, the TAP built and signed, the key read
  from `RELEASE_SIGNING_KEY` only) and `entry` (the release's flat catalog
  entry: download URLs of the release's assets, `min_os_version`,
  `changelog`, `publisher_key_id` the first 16 hex of
  `tools/production_public.key`). Assets: `<id>-<version>-esp32s3.tap`,
  `.tap.sig`, `.catalog-entry.json`, `SHA256SUMS`.
- **`.github/workflows/release-app.yml`**, on `*-v*`: `check` (lists the
  releases with `gh api --paginate --slurp`, refuses before anything is
  built), `build` (ESP-IDF's container, no `.env`, no secret, the app's
  cmake with the S3 toolchain), `release` (environment `release`; stage,
  **Sign the package** the only step given the key, `tap_package.py verify`
  against the committed key, entry, sums, draft, attest, publish), then
  `pages.yml`. The firmware release already calls Pages.
- **`build_catalog.py --releases`**: `catalog_from_releases()` takes a
  Releases API listing (one page or `--slurp`'s pages), leaves out drafts,
  prereleases and releases with no `*.catalog-entry.json` asset (upstream's
  kind), takes the newest firmware by version and the newest release of
  each app, and refuses an entry that disagrees with its tag or is not
  flat. Tested over `tools/testdata/releases_api_listing.json`.
  `pages.yml` writes it to `_site/store/catalog.json` on every build, a
  docs-only push included, and no longer copies the simulator's fixture
  catalog into `_site/apps/`. `app_entry()` is shared with the card
  catalog's `scan_artifacts()`.
- **The device**: `DEFAULT_CATALOG_URL` (and thistle-tk's) is
  `https://medaouadhi.github.io/thistle-os/store/catalog.json`; the
  simulator's card names it; a card's `config/appstore.json` still
  overrides it (test_appstore.py's cases all do).

Tests: `tools/test_release_app.py` (25), `test_build_catalog.py` (15),
`test_ci_workflows.py` (35) and every other `tools/test_*.py` pass;
actionlint (no shellcheck) is clean; `cargo test` 2018 passed; the
simulator's boot test and `test_appstore.py` pass, with a new case:
Notes released by `release_app.py`'s steps, catalogued by
`catalog_from_releases`, served at `/store/catalog.json`, listed by Refresh
and fetched through 302s, stopping at the signature as the simulator does
not trust the throwaway key. A local dry run built Notes for the S3 with
ESP-IDF 5.5.1 as the workflow does, and staged, signed and wrote the entry
with a throwaway key; `tap_package.py verify` passes against it and refuses
the committed key.

**Left, needing the user:**

- **A firmware release must exist before any app tag**: the check refuses
  an app tag while the repository has no firmware release (it has none
  now). Tag `v0.6.0-rc.1` first; an app with `min_os` up to `0.6.0-rc.1`
  can then follow.
- **To release an app**, raise `version` in its `manifest.json`, add the
  release, newest first, with its `changes` to `metadata.json`, commit, and
  push `<app>-v<version>`. The four apps' current versions (1.0.0, 2.0.0
  for Flashlight) can be tagged as they are.
- **Box 4's "served on the fork's Pages"** waits for a Pages deploy: a push
  to main, or a release's call. As ticket 04 found, the `github-pages`
  environment admits `main` only, so a release's call is refused at deploy
  until tag rules `v*` and `*-v*` are added to it.
- **Box 6, the T-Deck Max pass**: flash a laptop build of this branch, tag
  an app, then Refresh and install from the App Store. A laptop build
  trusts the release key, so the release's TAP installs.
