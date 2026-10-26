# Releases are GitHub Releases, and the catalog is built from them onto Pages

Decided 2026-10-22; not yet built.

This fork releases on its own, as its own publisher, without a website to host. Upstream's pipeline runs on wan0net's self-hosted runners, signs with their key and pushes the catalog to `wan0net/thistle-apps` with a deploy key; none of that works from a fork, and devices trusted wan0net's key, even ones built here.

A release is a pushed tag: `vX.Y.Z` for the firmware, `<app>-vX.Y.Z` for each App Store app (`notes-v1.0.3`), so a fix to one app needs no firmware release. A workflow on GitHub-hosted runners, free on a public repository, builds that commit for the ESP32-S3, signs with the release key, attaches the artifacts and artifact manifests to a draft GitHub Release with build provenance attestations, and then publishes it; immutable releases are on, so a published release never changes. The tag is the version: the firmware takes it through `git describe`, so a laptop build counts as the tag it was built from, and an app's tag must match its `manifest.json`. A tag with a suffix (`v0.6.0-rc.1`) is a prerelease, which devices are not offered.

The catalog is built from the latest release of each, on every Pages deploy, at a fixed URL on this repository's Pages. Releases are the record; the catalog is derived and holds nothing of its own. Devices check it once a day. Release asset URLs answer with a redirect to `release-assets.githubusercontent.com`, so the device's downloads follow redirects: at most five, HTTPS only, to any host, since what is installed is trusted for the publisher's signature and its SHA-256, never for where it came from.

There is one release key, Ed25519. It signs firmware and apps. It exists only as a secret of a `release` environment that admits release tags alone, and a tag ruleset lets only the owner create those tags. The `github-pages` environment admits those tags too, beside `main`, since a release's run deploys Pages from its tag. Released firmware trusts that key and no other; a laptop build also trusts the laptop's publisher key for apps, as before.

Released firmware compiles in nothing from `.env`. Its assets are public, so a key in the `.bin` is a published key. A device on released firmware reads its PodcastIndex, Cohere and AssistNow keys and the Hermes bridge's address from the card's `config/` files, which already override the built-in ones.

## Considered Options

**The catalog as a release asset** (`releases/latest/download/catalog.json`) fails once the firmware and apps release apart: only one release is the latest. **A catalog on a branch**, read through `raw.githubusercontent.com`, is a use of it as a CDN that GitHub discourages. **Copying the artifacts to Pages** to avoid the redirect fills a 1 GB site that each deploy replaces whole.

**A private repository with the keys compiled in** gets 2,000 Actions minutes a month, and every device would need a GitHub token to download a release. **release-please** needs Conventional Commit messages, which this repository does not write. **Upstream's manual dispatch**, given a tag and a CI run to copy from, takes two inputs where a tag alone says everything.

**An offline backup of the release key** was declined: if the key or the repository is lost, every device takes a new key by USB.

## Consequences

Every device moves to this publisher's key and catalog by one USB flash, the same one that takes ADR-0010's partition table. A device on released firmware cannot install an app signed on a laptop; that needs a laptop build of the firmware. Merges from upstream conflict in `.github/`.

Apps declare the oldest firmware they run on (`min_os`); devices hide releases they cannot run, and CI refuses an app tag whose `min_os` is newer than the latest firmware release.
