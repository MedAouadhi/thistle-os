#!/usr/bin/env python3
"""Every link a reader follows points at the fork, MedAouadhi/thistle-os, and
its Pages, not at upstream's wan0net repositories. What may still name wan0net
is listed below with its reason: the records of past work, Recovery (out of
scope until it is brought up on the fork), and the one line crediting the
upstream project. Run from the repository root."""

import subprocess
import unittest
from pathlib import Path

NAME = "wan0net"

# Paths, or folders ending in "/", that may name upstream, and why.
ALLOWED_PATHS = {
    ".scratch/": "specs and tickets, records of the work",
    "docs/adr/": "decision records",
    "docs/architecture-conversation-record.md": "a record of past work",
    "docs/rust-driver-inventory.md": "links to upstream's issues, where that work is tracked",
    "FABLE_IMPROVEMENTS.md": "a review of upstream at one commit",
    "recovery/": "Recovery's catalog URLs and keys stay until it is brought up",
    "docs/recovery.html": "describes Recovery's URLs, which are upstream's",
    "tools/test_ci_workflows.py": "checks the workflows name nothing of upstream's",
    "tools/test_upstream_links.py": "this test",
}

# Single lines that may name upstream, by file and a part of the line.
ALLOWED_LINES = {
    ("README.md", "is a fork of ThistleOS by wan0net"),
}


def allowed_path(path: str) -> bool:
    return any(path == p or (p.endswith("/") and path.startswith(p)) for p in ALLOWED_PATHS)


def stray_references() -> list[str]:
    files = subprocess.run(
        ["git", "grep", "-l", "-I", "-i", NAME],
        capture_output=True, text=True, check=False,
    ).stdout.split()
    stray = []
    for path in files:
        if allowed_path(path):
            continue
        for number, line in enumerate(Path(path).read_text(errors="replace").splitlines(), 1):
            if NAME not in line.lower():
                continue
            if any(path == f and part in line for f, part in ALLOWED_LINES):
                continue
            stray.append(f"{path}:{number}: {line.strip()[:120]}")
    return stray


class UpstreamLinkTests(unittest.TestCase):
    def test_no_stray_reference_to_upstream(self):
        stray = stray_references()
        self.assertEqual(stray, [], "point these at the fork, or allow them here with a reason:\n"
                         + "\n".join(stray))

    def test_the_upstream_project_is_credited(self):
        readme = Path("README.md").read_text()
        for _, part in ALLOWED_LINES:
            self.assertIn(part, readme)

    def test_every_allowed_path_exists(self):
        for path in ALLOWED_PATHS:
            with self.subTest(path=path):
                self.assertTrue(Path(path).exists(), "drop it from ALLOWED_PATHS")


if __name__ == "__main__":
    unittest.main()
