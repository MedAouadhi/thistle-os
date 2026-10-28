#!/usr/bin/env python3
"""The fork's workflows run on GitHub's hosted runners and publish nothing of
upstream's: no self-hosted runner, no wan0net/thistle-apps, no deploy key.
CI builds the firmware for the chips in its list, the ESP32-S3 alone, in
ESP-IDF's container, and refuses to build with a .env in its workspace, so
nothing from it reaches a firmware. Run from the repository root."""

import unittest
from pathlib import Path

import yaml

WORKFLOWS = Path(".github/workflows")


def load(path: Path) -> dict:
    # PyYAML reads the key `on` as True.
    return yaml.safe_load(path.read_text())


def runner_labels(job: dict) -> list[str]:
    runs_on = job.get("runs-on", [])
    return [runs_on] if isinstance(runs_on, str) else list(runs_on)


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.paths = sorted(WORKFLOWS.glob("*.yml"))
        self.assertTrue(self.paths)

    def test_every_job_runs_on_a_hosted_runner(self):
        for path in self.paths:
            for name, job in load(path)["jobs"].items():
                if "uses" in job:
                    continue  # a reusable workflow's jobs are checked in their file
                with self.subTest(workflow=path.name, job=name):
                    self.assertEqual(runner_labels(job), ["ubuntu-latest"])

    def test_nothing_of_upstreams_is_named(self):
        for path in self.paths:
            text = path.read_text()
            with self.subTest(workflow=path.name):
                self.assertNotIn("self-hosted", text)
                self.assertNotIn("wan0-ci", text)
                self.assertNotIn("thistle-apps", text)
                self.assertNotIn("THISTLE_APPS_DEPLOY_KEY", text)

    def test_upstreams_release_and_package_workflows_are_gone(self):
        self.assertFalse((WORKFLOWS / "release.yml").exists())
        self.assertFalse((WORKFLOWS / "apps.yml").exists())
        self.assertFalse((WORKFLOWS / "build.yml").exists())


class FirmwareJobTests(unittest.TestCase):
    def setUp(self):
        self.ci = load(WORKFLOWS / "ci.yml")
        self.job = self.ci["jobs"]["firmware"]
        self.steps = self.job["steps"]

    def test_runs_on_a_push_to_main_and_every_pull_request(self):
        triggers = self.ci[True]
        self.assertEqual(triggers["push"]["branches"], ["main"])
        self.assertIn("pull_request", triggers)
        self.assertNotIn("branches", triggers["pull_request"] or {})

    def test_builds_the_s3_alone_from_a_list(self):
        self.assertEqual(self.job["strategy"]["matrix"]["chip"], ["esp32s3"])
        self.assertEqual(self.job["container"]["image"], "espressif/idf:v5.5")
        build = next(s for s in self.steps if s.get("name") == "Build firmware")
        self.assertIn('idf.py set-target "$CHIP"', build["run"])
        self.assertEqual(build["env"]["CHIP"], "${{ matrix.chip }}")

    def test_adding_a_chip_is_one_line(self):
        text = (WORKFLOWS / "ci.yml").read_text()
        self.assertIn("        chip:\n          - esp32s3\n", text)

    def test_refuses_a_dotenv_before_building(self):
        names = [s.get("name") for s in self.steps]
        guard = names.index("No .env in the workspace")
        self.assertLess(guard, names.index("Build firmware"))
        run = self.steps[guard]["run"]
        self.assertIn("-e .env", run)
        self.assertIn("exit 1", run)

    def test_checks_the_partition_sizes(self):
        runs = "\n".join(s.get("run", "") for s in self.steps)
        self.assertIn("tools/check_partition_sizes.py", runs)
        self.assertIn("--firmware build/thistle_os.bin", runs)

    def test_signs_nothing(self):
        text = (WORKFLOWS / "ci.yml").read_text()
        self.assertNotIn("secrets.THISTLE_SIGNING_KEY", text)


def step_named(steps, name):
    matches = [s for s in steps if s.get("name") == name]
    if len(matches) != 1:
        raise AssertionError(f"expected one step named {name!r}, found {len(matches)}")
    return matches[0]


class FirmwareReleaseTests(unittest.TestCase):
    """release-firmware.yml: a tag vX.Y.Z builds that commit, signs it with
    the release key, publishes a release from a draft, attests it, and calls
    Pages (spec, "The release workflows")."""

    def setUp(self):
        self.path = WORKFLOWS / "release-firmware.yml"
        self.text = self.path.read_text()
        self.workflow = load(self.path)
        self.jobs = self.workflow["jobs"]
        self.build = self.jobs["build"]
        self.release = self.jobs["release"]
        self.release_steps = self.release["steps"]
        self.release_names = [s.get("name") for s in self.release_steps]

    def test_runs_on_firmware_tags_only(self):
        self.assertEqual(self.workflow[True], {"push": {"tags": ["v*"]}})

    def test_reads_nothing_and_writes_nothing_by_default(self):
        self.assertEqual(self.workflow["permissions"], {})

    def test_builds_the_tagged_commit_with_its_tags_and_no_env(self):
        self.assertEqual(self.build["container"]["image"], "espressif/idf:v5.5")
        steps = self.build["steps"]
        checkout = steps[0]
        self.assertTrue(checkout["uses"].startswith("actions/checkout@"))
        # git describe needs the tags; a shallow clone has none.
        self.assertEqual(checkout["with"]["fetch-depth"], 0)
        self.assertNotIn("ref", checkout["with"])
        names = [s.get("name") for s in steps]
        self.assertLess(names.index("No .env in the workspace"), names.index("Build firmware"))
        build = step_named(steps, "Build firmware")
        self.assertIn('idf.py set-target "$CHIP"', build["run"])

    def test_builds_the_s3_from_a_list(self):
        self.assertEqual(self.build["strategy"]["matrix"]["chip"], ["esp32s3"])
        self.assertIn("        chip:\n          - esp32s3\n", self.text)

    def test_the_firmware_reports_the_tag_as_its_version(self):
        check = step_named(self.build["steps"], "The firmware's version is the tag")
        self.assertIn("tools/firmware_version.sh", check["run"])
        self.assertIn("project_description.json", check["run"])
        self.assertIn("exit 1", check["run"])

    def test_checks_the_size_and_merges_a_full_flash_image(self):
        runs = "\n".join(s.get("run", "") for s in self.build["steps"])
        self.assertIn("tools/check_partition_sizes.py", runs)
        self.assertIn("merge_bin", runs)
        self.assertIn("@flash_args", runs)

    def test_the_build_job_has_no_secrets(self):
        self.assertNotIn("environment", self.build)
        self.assertNotIn("secrets.", yaml.safe_dump(self.build))

    def test_only_the_signing_step_receives_the_key(self):
        # The key is the only secret named, once; the token is github.token.
        self.assertEqual(self.text.count("secrets."), 1)
        self.assertEqual(self.text.count("secrets.RELEASE_SIGNING_KEY"), 1)
        sign = step_named(self.release_steps, "Sign the artifact manifest")
        self.assertEqual(sign["env"]["RELEASE_SIGNING_KEY"], "${{ secrets.RELEASE_SIGNING_KEY }}")
        self.assertIn("tools/release_firmware.py sign", sign["run"])
        self.assertEqual(self.release["environment"], "release")

    def test_the_manifest_is_checked_against_the_committed_key(self):
        verify = step_named(self.release_steps, "Check the signature against the committed key")
        self.assertIn("tools/sign.py verify", verify["run"])
        self.assertIn("--pubkey tools/production_public.key", verify["run"])
        self.assertLess(self.release_names.index("Sign the artifact manifest"),
                        self.release_names.index("Check the signature against the committed key"))

    def test_a_draft_gets_every_asset_then_attestations_then_is_published(self):
        order = ["Stage the release", "Sign the artifact manifest",
                 "Check the signature against the committed key", "Write SHA256SUMS",
                 "Create the draft release", "Attest provenance", "Publish the release"]
        positions = [self.release_names.index(name) for name in order]
        self.assertEqual(positions, sorted(positions))

        create = step_named(self.release_steps, "Create the draft release")["run"]
        self.assertIn("gh release create", create)
        self.assertIn("--draft", create)
        self.assertIn("--verify-tag", create)
        self.assertIn("--notes-file", create)
        self.assertIn("dist/*", create)

        attest = step_named(self.release_steps, "Attest provenance")
        self.assertTrue(attest["uses"].startswith("actions/attest@"))
        self.assertEqual(attest["with"]["subject-path"], "dist/*")

        publish = step_named(self.release_steps, "Publish the release")["run"]
        self.assertIn("gh release edit", publish)
        self.assertIn("--draft=false", publish)
        self.assertNotIn("upload", publish)

    def test_a_suffixed_tag_is_a_prerelease(self):
        create = step_named(self.release_steps, "Create the draft release")["run"]
        self.assertIn("--prerelease", create)
        tag = step_named(self.release_steps, "Read the tag")["run"]
        self.assertIn("tools/release_firmware.py tag", tag)

    def test_the_release_job_may_write_releases_and_attest(self):
        self.assertEqual(self.release["permissions"], {
            "contents": "write", "id-token": "write", "attestations": "write"})
        self.assertEqual(self.release["needs"], "build")

    def test_then_calls_pages(self):
        pages = self.jobs["pages"]
        self.assertEqual(pages["uses"], "./.github/workflows/pages.yml")
        self.assertEqual(pages["needs"], "release")
        self.assertEqual(pages["permissions"], {
            "contents": "read", "pages": "write", "id-token": "write"})
        self.assertIn("workflow_call", load(WORKFLOWS / "pages.yml")[True])

    def test_no_workflow_but_the_release_workflows_names_the_release_key(self):
        releases = {"release-firmware.yml", "release-app.yml"}
        for path in sorted(WORKFLOWS.glob("*.yml")):
            if path.name not in releases:
                with self.subTest(workflow=path.name):
                    self.assertNotIn("RELEASE_SIGNING_KEY", path.read_text())


class AppReleaseTests(unittest.TestCase):
    """release-app.yml: a tag <app>-vX.Y.Z checks the tag, builds that App
    Store app for the S3, signs its ELF and TAP with the release key, and
    publishes a release from a draft as the firmware's release does."""

    def setUp(self):
        self.path = WORKFLOWS / "release-app.yml"
        self.text = self.path.read_text()
        self.workflow = load(self.path)
        self.jobs = self.workflow["jobs"]
        self.release = self.jobs["release"]
        self.release_steps = self.release["steps"]
        self.release_names = [s.get("name") for s in self.release_steps]

    def test_runs_on_app_tags_only(self):
        self.assertEqual(self.workflow[True], {"push": {"tags": ["*-v*"]}})

    def test_reads_nothing_and_writes_nothing_by_default(self):
        self.assertEqual(self.workflow["permissions"], {})

    def test_the_tag_is_checked_against_the_releases_before_anything_is_built(self):
        check = self.jobs["check"]
        self.assertEqual(check["permissions"], {"contents": "read"})
        runs = "\n".join(s.get("run", "") for s in check["steps"])
        self.assertIn('gh api --paginate --slurp "repos/$GITHUB_REPOSITORY/releases"', runs)
        self.assertIn("tools/release_app.py check", runs)
        self.assertEqual(self.jobs["build"]["needs"], "check")

    def test_builds_the_app_for_the_s3_in_espidfs_container_with_no_env(self):
        build = self.jobs["build"]
        self.assertEqual(build["container"]["image"], "espressif/idf:v5.5")
        self.assertNotIn("environment", build)
        self.assertNotIn("secrets.", yaml.safe_dump(build))
        names = [s.get("name") for s in build["steps"]]
        self.assertLess(names.index("No .env in the workspace"), names.index("Build the app"))
        run = step_named(build["steps"], "Build the app")["run"]
        self.assertIn("standalone_apps/$APP", run)
        self.assertIn("toolchain-esp32s3.cmake", run)

    def test_only_the_signing_step_receives_the_key(self):
        self.assertEqual(self.text.count("secrets."), 1)
        self.assertEqual(self.text.count("secrets.RELEASE_SIGNING_KEY"), 1)
        sign = step_named(self.release_steps, "Sign the package")
        self.assertEqual(sign["env"]["RELEASE_SIGNING_KEY"], "${{ secrets.RELEASE_SIGNING_KEY }}")
        self.assertIn("tools/release_app.py sign", sign["run"])
        self.assertEqual(self.release["environment"], "release")

    def test_the_package_is_checked_against_the_committed_key(self):
        verify = step_named(self.release_steps, "Check the signatures against the committed key")
        self.assertIn("tools/tap_package.py verify", verify["run"])
        self.assertIn("--pubkey tools/production_public.key", verify["run"])

    def test_a_draft_gets_every_asset_then_attestations_then_is_published(self):
        order = ["Stage the release", "Sign the package",
                 "Check the signatures against the committed key", "Write the catalog entry",
                 "Write SHA256SUMS", "Create the draft release", "Attest provenance",
                 "Publish the release"]
        positions = [self.release_names.index(name) for name in order]
        self.assertEqual(positions, sorted(positions))
        create = step_named(self.release_steps, "Create the draft release")["run"]
        for flag in ("gh release create", "--draft", "--verify-tag", "--notes-file",
                     "dist/*", "--prerelease"):
            self.assertIn(flag, create)
        attest = step_named(self.release_steps, "Attest provenance")
        self.assertEqual(attest["with"]["subject-path"], "dist/*")
        publish = step_named(self.release_steps, "Publish the release")["run"]
        self.assertIn("--draft=false", publish)
        self.assertIn("--latest=false", publish)

    def test_the_release_job_may_write_releases_and_attest(self):
        self.assertEqual(self.release["permissions"], {
            "contents": "write", "id-token": "write", "attestations": "write"})

    def test_then_calls_pages(self):
        pages = self.jobs["pages"]
        self.assertEqual(pages["uses"], "./.github/workflows/pages.yml")
        self.assertEqual(pages["needs"], "release")


class PagesTests(unittest.TestCase):
    def setUp(self):
        self.pages = load(WORKFLOWS / "pages.yml")
        self.text = (WORKFLOWS / "pages.yml").read_text()

    def test_deploys_on_a_push_to_main_only(self):
        self.assertEqual(self.pages[True]["push"]["branches"], ["main"])
        self.assertEqual(self.pages["jobs"]["deploy"]["if"], "github.event_name == 'push'")

    def test_names_the_forks_own_pages(self):
        self.assertNotIn("wan0net", self.text)
        self.assertIn("github.repository_owner", self.text)

    def test_every_deploy_serves_the_catalog_built_from_the_releases(self):
        steps = self.pages["jobs"]["build"]["steps"]
        store = step_named(steps, "Build the store catalog from the releases")
        self.assertIn('gh api --paginate --slurp "repos/$GITHUB_REPOSITORY/releases"', store["run"])
        self.assertIn("tools/build_catalog.py --releases", store["run"])
        self.assertIn("--output _site/store/catalog.json", store["run"])
        self.assertEqual(store["env"]["GH_TOKEN"], "${{ github.token }}")
        names = [s.get("name") for s in steps]
        self.assertLess(names.index("Build the store catalog from the releases"),
                        names.index("Upload Pages artifact"))
        # The catalog holds nothing of its own: no fixture is copied in.
        self.assertNotIn("appstore/catalog.json", self.text)

    def test_each_run_deploys_a_build_version_of_its_own(self):
        deploy = step_named(self.pages["jobs"]["deploy"]["steps"], "Deploy")
        self.assertEqual(deploy["env"]["GITHUB_SHA"],
                         "${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}")


if __name__ == "__main__":
    unittest.main()
