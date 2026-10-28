#!/usr/bin/env python3
"""tools/firmware_version.sh gives the firmware's version, its tag: what
`git describe` says, firmware tags (`v<digit>...`) only, the `v` dropped.
Run from the repository root."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "firmware_version.sh"


def git(repo, *args):
    env = dict(os.environ, GIT_AUTHOR_DATE="2026-01-01T00:00:00",
               GIT_COMMITTER_DATE="2026-01-01T00:00:00")
    subprocess.run(["git", "-C", str(repo), *args], check=True,
                   capture_output=True, env=env)


def commit(repo, name):
    (repo / name).write_text(name)
    git(repo, "add", name)
    git(repo, "-c", "user.name=t", "-c", "user.email=t@example.com",
        "commit", "-q", "-m", name)


def version(repo):
    out = subprocess.run(["sh", str(SCRIPT), str(repo)], check=True,
                         capture_output=True, text=True)
    return out.stdout.strip()


class FirmwareVersionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        git(self.repo, "init", "-q")
        commit(self.repo, "a")

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_tagged_commit_is_its_tag_without_the_v(self):
        git(self.repo, "tag", "v0.6.0")
        self.assertEqual(version(self.repo), "0.6.0")

    def test_a_release_candidate_tag(self):
        git(self.repo, "tag", "v0.6.0-rc.1")
        self.assertEqual(version(self.repo), "0.6.0-rc.1")

    def test_commits_after_a_tag_and_changes_are_described(self):
        git(self.repo, "tag", "v0.5.2")
        commit(self.repo, "b")
        (self.repo / "b").write_text("changed")
        self.assertRegex(version(self.repo), r"^0\.5\.2-1-g[0-9a-f]+-dirty$")

    def test_an_app_tag_never_becomes_the_firmwares_version(self):
        git(self.repo, "tag", "v0.5.2")
        commit(self.repo, "b")
        git(self.repo, "tag", "notes-v1.0.1")
        git(self.repo, "tag", "sprint9-complete")
        self.assertRegex(version(self.repo), r"^0\.5\.2-1-g[0-9a-f]+$")

    def test_no_firmware_tag_in_reach(self):
        git(self.repo, "tag", "notes-v1.0.1")
        self.assertEqual(version(self.repo), "0.0.0-unknown")

    def test_not_a_repository(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(version(Path(empty)), "0.0.0-unknown")


if __name__ == "__main__":
    unittest.main()
