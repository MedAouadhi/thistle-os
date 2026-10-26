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


class PagesTests(unittest.TestCase):
    def test_deploys_on_a_push_to_main_only(self):
        pages = load(WORKFLOWS / "pages.yml")
        self.assertEqual(pages[True]["push"]["branches"], ["main"])
        self.assertEqual(pages["jobs"]["deploy"]["if"], "github.event_name == 'push'")

    def test_names_the_forks_own_pages(self):
        text = (WORKFLOWS / "pages.yml").read_text()
        self.assertNotIn("wan0net", text)
        self.assertIn("github.repository_owner", text)


if __name__ == "__main__":
    unittest.main()
