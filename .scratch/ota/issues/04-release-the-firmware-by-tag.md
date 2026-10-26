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

- [ ] Version reading and comparison are tested on the host: plain tags,
      `git describe` forms, `-dirty`, release candidates (`0.6.0-rc.1` is
      below `0.6.0`, and `0.6.0-rc.1-3-gabc` counts as `0.6.0-rc.1`), and
      malformed input.
- [ ] `git describe` matches firmware tags only (`v[0-9]*`), so an app tag
      such as `notes-v1.0.1` never becomes the firmware's version.
- [ ] The boot log, Settings and `min_os` checks use the version; a laptop
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
