# 02: CI on GitHub-hosted runners

**What to build:** Every push to main and every pull request on the fork runs
on GitHub's free hosted runners: the ESP32-S3 firmware built in the
`espressif/idf:v5.5` container with no `.env`, its partition size check, and
the host tests that the tests workflow runs today (Rust kernel, Python tools,
simulator). The Pages workflow moves to hosted runners too. Upstream's
release and package workflows, which run on wan0net's self-hosted runners and
publish to `wan0net/thistle-apps`, are removed with the tests that asserted on
them. Released firmware will be built the same way, so this ticket also
checks that a build without keys still tells the user what each keyed
feature lacks. Spec: `../spec.md`, "The release workflows" and "No keys in
released firmware". ADR-0011.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [ ] A push to the fork produces a green run with no job waiting for a
      runner.
- [ ] No workflow names a self-hosted runner label, checks out or deploys to
      `wan0net/thistle-apps`, or reads `THISTLE_APPS_DEPLOY_KEY`.
- [ ] The firmware job builds for the S3 alone, from a chip list where adding
      a chip is one line, and fails if a `.env` exists in its workspace.
- [ ] Tools and tests that existed only for the removed workflows (the
      release provenance validator, the package workflow assertions, the
      five-chip package matrix check) are removed or rewritten to match.
- [ ] Built without `.env`, the simulator's Podcasts says it has no key,
      Dictation refuses with a message naming what is missing, and Hermes
      shows its Address page; each covered by a test case.
- [ ] The Pages workflow deploys the docs and the WASM simulator to the
      fork's Pages (Pages served from GitHub Actions is a repository setting
      the user turns on; say so in the ticket's Comments if it is off).
