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

- [ ] An app tag is refused when its app is not one of the four App Store
      apps, when its version differs from the package manifest's, or when its
      `min_os` is newer than the newest firmware release, prereleases
      included (a change from the spec, agreed: an app may need a release
      candidate's kernel, and devices hide what they cannot run).
- [ ] The release sequence is computed from the version (major x 10^6 +
      minor x 10^3 + patch) and written into the package; the notes come from
      the app's metadata for that version.
- [ ] App and firmware releases both call the Pages workflow when they
      publish, since a release made with the workflow's token starts no other
      workflow.
- [ ] The catalog builder, tested over a recorded Releases API listing,
      takes the newest firmware by version (not date) and the newest release
      of each app, leaves out drafts and prereleases, and writes flat
      firmware entries; its output is served at `store/catalog.json` on the
      fork's Pages, and a docs-only deploy rebuilds the same catalog.
- [ ] The device's default catalog URL is the fork's Pages catalog, a card's
      `config/appstore.json` still overrides it, and the simulator's card
      points at it too.
- [ ] On the T-Deck Max running a laptop build, the App Store's Refresh lists
      the released app from the fork's Pages and installs it.
