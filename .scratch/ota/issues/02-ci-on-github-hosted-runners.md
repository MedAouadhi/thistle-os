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
- [x] No workflow names a self-hosted runner label, checks out or deploys to
      `wan0net/thistle-apps`, or reads `THISTLE_APPS_DEPLOY_KEY`.
- [x] The firmware job builds for the S3 alone, from a chip list where adding
      a chip is one line, and fails if a `.env` exists in its workspace.
- [x] Tools and tests that existed only for the removed workflows (the
      release provenance validator, the package workflow assertions, the
      five-chip package matrix check) are removed or rewritten to match.
- [x] Built without `.env`, the simulator's Podcasts says it has no key,
      Dictation refuses with a message naming what is missing, and Hermes
      shows its Address page; each covered by a test case.
- [ ] The Pages workflow deploys the docs and the WASM simulator to the
      fork's Pages (Pages served from GitHub Actions is a repository setting
      the user turns on; say so in the ticket's Comments if it is off).

## Comments

**2026-10-26, implementer.** Done on branch `worktree-agent-a197e9b5d2e1be413`;
nothing pushed and no GitHub setting changed.

- `build.yml` is now `ci.yml`, on a push to main and every pull request:
  `firmware` builds each chip of `matrix.chip` (one a line: `esp32s3`) in
  `espressif/idf:v5.5`, first failing if `.env` exists, then
  `check_partition_sizes.py`, and uploads the image, bootloader, partition
  table and `flasher_args.json`. It signs nothing; the release workflows
  (tickets 04, 05) will. Recovery, cargo-audit, Semgrep and Trivy stay, on
  `ubuntu-latest`. `tests.yml` and `pages.yml` run on `ubuntu-latest`;
  `tests.yml` now runs on every pull request, installs `tools/requirements.txt`
  through `actions/setup-python` (Ubuntu 24.04's pip refuses system installs,
  and the Notes packaging in `test_dictation.py` needs `cryptography`), and
  runs `test_dictation.py`, which no workflow ran. `pages.yml` and the board
  catalog check name `https://<owner>.github.io/<repo>/config/boards`.
- Removed: `release.yml`, `apps.yml` (the deploy to `wan0net/thistle-apps`,
  `THISTLE_APPS_DEPLOY_KEY`), `validate_release.py` and its test,
  `validate_package_matrix.py` and its test, `test_package_workflow.py`, and
  the workflow checks of `test_target_matrix.py`, which now checks only that
  CI's chips are supported ones. `tools/test_ci_workflows.py` holds the rules
  above (hosted runners only, nothing of upstream's, the S3 list, the `.env`
  guard before the build, the size check, Pages only on a push to main).
- `test_wasm_migration.py` failed on `ota` already: it looked for
  per-app guards that `wasm/main.c` replaced with `lvgl_apps_register_builtin()`.
  It now checks that.
- No keys: Podcasts (`says_it_has_no_key`: Trending logs `no podcasts: No
  PodcastIndex key. Set one up from the home page.` and sends nothing) and
  Dictation (`says_it_has_no_key`: refuses before recording with `Dictation
  needs a Cohere API key: config/dictation.json`) have new cases; Hermes'
  `asks-for-an-address-and-keeps-it` already covered its Address page. Each
  runs only on a build without the key from `.env`, as CI's is; on a laptop
  with `.env` they print SKIP.
- `sim_vfs.c` passed 512-byte buffers to `realpath()`; a Release (fortified)
  simulator build aborted at start. They are `PATH_MAX` now.

Tests run here: every `tools/test_*.py` in `tests.yml` passes, and the board
catalog builds and validates; the simulator builds without `.env` and its
boot test passes; `test_dictation.py` 7/7 and `test_hermes.py`'s address
case pass; `test_podcasts.py`'s new case passes, while its other cases
failed at random in runs alongside other sessions' simulators, which share
`/tmp/thistle_sdcard` (one quiet run: 7/8). actionlint was not available;
the workflows parse as YAML in `test_ci_workflows.py`.

**Left for the user:**

- Push to the fork and check the first run is green (first box). Not tried
  on a hosted runner: the firmware job's disk space and time in the IDF
  container, `cargo audit` and Trivy (both fail on any new advisory), and
  Semgrep, all as upstream had them.
- **GitHub Pages is off on MedAouadhi/thistle-os** (`gh api
  repos/MedAouadhi/thistle-os/pages` answers 404, `has_pages: false`), so
  the Pages deploy (last box) fails until Settings → Pages → Source is set to
  "GitHub Actions". Actions are enabled on the fork; the repository is
  public.
