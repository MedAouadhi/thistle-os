# 11: Every release reaches the catalog

**What to build:** A release's Pages deploy publishes its catalog even when
the tagged commit has been deployed before. Today Pages keeps the files it
has for a build version it has already deployed, and `actions/deploy-pages`
sends the commit, `GITHUB_SHA`, as that version. A step's `env:` cannot
change it: the runner keeps its own `GITHUB_SHA`, as the deploy log of
d6f959e shows (`"pages_build_version"` is the bare commit). So an app
tagged on main's head, which a push to main has already deployed, leaves
its release out of the catalog until the next deploy from another commit.

**Seen:** 2026-10-27, `v0.6.0` and then the four app releases were tagged
on 4212a7d. Each app run's deploy reported success; the site kept serving
the `v0.6.0` deploy's files (`last-modified` 19:59). `notes-v1.0.1`, on a
new commit, published them.

**The likely fix:** deploy through the REST API, as the action does, with
a version of the run's own: `POST /repos/{owner}/{repo}/pages/deployments`
with the `github-pages` artifact's id, the job's OIDC token, and
`pages_build_version` set to `<sha>-<run id>-<attempt>`, the API's
"unique string"; then poll the deployment until it succeeds or fails.
Prove it by tagging a release on an already deployed commit and seeing
the catalog change. `tools/test_ci_workflows.py` checks the step.

**Until then:** tag a commit Pages has not deployed (CLAUDE.md, Releases
and updates).

**Blocked by:** none

**Status:** ready-for-agent
