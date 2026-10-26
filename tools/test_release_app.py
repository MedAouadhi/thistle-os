#!/usr/bin/env python3
"""An app release's own steps (tools/release_app.py): which tags release an
app, the checks a tag must pass against the app's package manifest and the
repository's firmware releases, the release sequence and notes written into
the package, the signed TAP and the release's catalog entry. Run from the
repository root."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_app as ra  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import (  # noqa: E402
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

TOOL = Path(__file__).resolve().parent / "release_app.py"
REPO = "MedAouadhi/thistle-os"


def firmware_release(tag, draft=False, prerelease=False):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease, "assets": []}


def write_app(root, name="notes", version="1.0.1", min_os="0.5.0",
              releases=None):
    """A standalone_apps/<name>/ folder as the repository keeps one."""
    folder = root / name
    folder.mkdir(parents=True)
    app_id = f"com.thistle.{name}"
    (folder / "manifest.json").write_text(json.dumps({
        "type": "app", "id": app_id, "name": name.title(), "version": version,
        "release_sequence": 1, "author": "ThistleOS", "description": f"{name} app",
        "category": "productivity", "min_os": min_os, "arch": "esp32s3",
        "entry": f"{name}.app.elf", "permissions": ["storage"],
        "background": False, "min_memory_kb": 48,
    }, indent=2))
    if releases is None:
        releases = [
            {"version": version, "release_sequence": 1, "released": "2026-10-26",
             "changes": ["Saves on every keystroke", "Fixed the cursor"],
             "min_os": min_os},
            {"version": "1.0.0", "release_sequence": 1, "released": "2026-08-29",
             "changes": ["First release"]},
        ]
    (folder / "metadata.json").write_text(json.dumps({
        "schema": "thistle.app.metadata/v1", "id": app_id,
        "category": "productivity", "summary": f"{name}", "description": f"{name}",
        "license": "BSD-3-Clause", "releases": releases,
    }, indent=2))
    return folder


class TagTests(unittest.TestCase):
    def test_an_app_tag_names_a_store_app_and_a_version(self):
        release = ra.parse_tag("notes-v1.0.1")
        self.assertEqual((release.app, release.version, release.prerelease),
                         ("notes", "1.0.1", False))

    def test_a_suffixed_app_tag_is_a_prerelease(self):
        release = ra.parse_tag("weather-v2.1.0-rc.1")
        self.assertEqual((release.app, release.version, release.prerelease),
                         ("weather", "2.1.0-rc.1", True))

    def test_every_store_app_has_tags(self):
        for app in ("notes", "weather", "wifiscanner", "flashlight"):
            with self.subTest(app=app):
                self.assertEqual(ra.parse_tag(f"{app}-v1.2.3").app, app)

    def test_an_app_outside_the_store_is_refused(self):
        for tag in ("hello-v1.0.0", "terminal-v1.0.0", "kbd_tca8418-v1.0.0"):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "App Store"):
                ra.parse_tag(tag)

    def test_malformed_tags_are_refused(self):
        for tag in ("v0.6.0", "notes-1.0.0", "notes-v1.0", "notes-v01.0.0",
                    "notes-v1.0.0-rc-1", "notes-v1.0.0+build", "-v1.0.0"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                ra.parse_tag(tag)


class CheckTests(unittest.TestCase):
    """A tag is refused before anything is built when the package and the
    firmware releases disagree with it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_tag_matching_the_manifest_and_a_released_firmware_passes(self):
        app = write_app(self.root, version="1.0.1", min_os="0.6.0")
        release = ra.check("notes-v1.0.1", app, [firmware_release("v0.6.0")])
        self.assertEqual(release.version, "1.0.1")

    def test_a_version_other_than_the_manifests_is_refused(self):
        app = write_app(self.root, version="1.0.0")
        with self.assertRaisesRegex(ValueError, "1.0.0"):
            ra.check("notes-v1.0.1", app, [firmware_release("v0.6.0")])

    def test_a_min_os_newer_than_every_firmware_release_is_refused(self):
        app = write_app(self.root, version="1.0.1", min_os="0.7.0")
        with self.assertRaisesRegex(ValueError, "0.7.0"):
            ra.check("notes-v1.0.1", app,
                     [firmware_release("v0.6.0"), firmware_release("v0.6.1")])

    def test_a_release_candidate_counts_as_a_firmware_release(self):
        # Agreed change from the spec: an app may follow a release candidate.
        app = write_app(self.root, version="1.0.1", min_os="0.7.0-rc.1")
        ra.check("notes-v1.0.1", app,
                 [firmware_release("v0.6.0"), firmware_release("v0.7.0-rc.1", prerelease=True)])

    def test_newest_is_by_version_and_ignores_drafts_and_app_releases(self):
        app = write_app(self.root, version="1.0.1", min_os="0.10.0")
        listing = [firmware_release("v0.10.0"), firmware_release("v0.9.0")]
        ra.check("notes-v1.0.1", app, listing)
        # A draft is not released, and an app's tag is not a firmware's.
        listing = [firmware_release("v0.9.0"), firmware_release("v0.10.0", draft=True),
                   firmware_release("notes-v2.0.0")]
        with self.assertRaisesRegex(ValueError, "0.9.0"):
            ra.check("notes-v1.0.1", app, listing)

    def test_a_release_candidate_is_below_its_release(self):
        app = write_app(self.root, version="1.0.1", min_os="0.7.0")
        with self.assertRaises(ValueError):
            ra.check("notes-v1.0.1", app, [firmware_release("v0.7.0-rc.2")])

    def test_no_firmware_release_at_all_is_refused(self):
        app = write_app(self.root, version="1.0.1", min_os="0.3.0")
        with self.assertRaisesRegex(ValueError, "no firmware release"):
            ra.check("notes-v1.0.1", app, [])


class StageTests(unittest.TestCase):
    """The package is written with the sequence its version gives, and the
    notes come from the app's metadata for that version."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.staged = self.root / "staged"

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_release_sequence_is_computed_from_the_version(self):
        self.assertEqual(ra.release_sequence("1.0.1"), 1_000_001)
        self.assertEqual(ra.release_sequence("2.0.0"), 2_000_000)
        self.assertEqual(ra.release_sequence("0.12.345"), 12_345)
        self.assertEqual(ra.release_sequence("1.2.3-rc.1"), 1_002_003)
        for version in ("1.1000.0", "1.0.1000"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                ra.release_sequence(version)

    def test_the_sequence_is_written_into_manifest_and_metadata(self):
        app = write_app(self.root, version="1.0.1")
        ra.stage("notes-v1.0.1", app, self.staged)
        manifest = json.loads((self.staged / "manifest.json").read_text())
        metadata = json.loads((self.staged / "metadata.json").read_text())
        self.assertEqual(manifest["release_sequence"], 1_000_001)
        self.assertEqual(metadata["releases"][0]["release_sequence"], 1_000_001)
        # The rest is as the repository has it.
        self.assertEqual(manifest["id"], "com.thistle.notes")
        self.assertEqual(metadata["releases"][1]["version"], "1.0.0")

    def test_the_repositorys_own_files_are_left_alone(self):
        app = write_app(self.root, version="1.0.1")
        before = (app / "manifest.json").read_text()
        ra.stage("notes-v1.0.1", app, self.staged)
        self.assertEqual((app / "manifest.json").read_text(), before)

    def test_the_notes_are_the_metadatas_changes_for_the_version(self):
        app = write_app(self.root, version="1.0.1")
        notes = ra.stage("notes-v1.0.1", app, self.staged)
        self.assertEqual(notes, "- Saves on every keystroke\n- Fixed the cursor\n")

    def test_metadata_without_the_version_is_refused(self):
        app = write_app(self.root, version="1.0.1", releases=[
            {"version": "1.0.0", "release_sequence": 1, "released": "2026-08-29",
             "changes": ["First release"]}])
        with self.assertRaisesRegex(ValueError, "metadata.json"):
            ra.stage("notes-v1.0.1", app, self.staged)

    def test_the_version_must_be_the_metadatas_newest(self):
        app = write_app(self.root, version="1.0.1", releases=[
            {"version": "1.0.2", "release_sequence": 1, "released": "2026-10-27",
             "changes": ["Later"]},
            {"version": "1.0.1", "release_sequence": 1, "released": "2026-10-26",
             "changes": ["Now"]}])
        with self.assertRaisesRegex(ValueError, "newest"):
            ra.stage("notes-v1.0.1", app, self.staged)

    def test_a_release_with_no_changes_is_refused(self):
        app = write_app(self.root, version="1.0.1", releases=[
            {"version": "1.0.1", "release_sequence": 1, "released": "2026-10-26",
             "changes": []}])
        with self.assertRaisesRegex(ValueError, "changes"):
            ra.stage("notes-v1.0.1", app, self.staged)


def run_tool(*args, env=None, cwd=None):
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True,
                          text=True, env=env, cwd=cwd, check=False)


class ReleaseStepsTests(unittest.TestCase):
    """The steps as the workflow runs them: stage, sign with a key from the
    environment, then the catalog entry."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.app = write_app(self.root / "apps", version="1.0.1", min_os="0.6.0")
        self.elf = self.root / "notes.app.elf"
        self.elf.write_bytes(b"\x7fELF notes")
        self.license = self.root / "LICENSE"
        self.license.write_text("BSD-3-Clause\n")
        self.staged = self.root / "staged"
        self.dist = self.root / "dist"
        self.notes = self.root / "notes.md"
        self.key = Ed25519PrivateKey.generate()
        self.public = self.key.public_key().public_bytes_raw()
        self.pubkey = self.root / "release_public.key"
        self.pubkey.write_bytes(self.public)
        self.key_hex = self.key.private_bytes_raw().hex()

    def tearDown(self):
        self.tmp.cleanup()

    def stage(self):
        result = run_tool("stage", "--tag", "notes-v1.0.1", "--app-dir", str(self.app),
                          "--staged", str(self.staged), "--notes", str(self.notes))
        self.assertEqual(result.returncode, 0, result.stderr)

    def sign(self, key_hex=None):
        env = dict(os.environ)
        env.pop("RELEASE_SIGNING_KEY", None)
        if key_hex is not None:
            env["RELEASE_SIGNING_KEY"] = key_hex
        return run_tool("sign", "--tag", "notes-v1.0.1", "--staged", str(self.staged),
                        "--elf", str(self.elf), "--license", str(self.license),
                        "--dist", str(self.dist), env=env)

    def entry(self):
        result = run_tool("entry", "--tag", "notes-v1.0.1", "--repo", REPO,
                          "--staged", str(self.staged), "--dist", str(self.dist),
                          "--notes", str(self.notes), "--updated", "2026-10-26",
                          "--pubkey", str(self.pubkey))
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads((self.dist / "com.thistle.notes-1.0.1-esp32s3.catalog-entry.json")
                          .read_text())

    def test_sign_builds_a_tap_signed_with_the_release_key(self):
        self.stage()
        result = self.sign(self.key_hex)
        self.assertEqual(result.returncode, 0, result.stderr)
        tap = self.dist / "com.thistle.notes-1.0.1-esp32s3.tap"
        public = Ed25519PublicKey.from_public_bytes(self.public)
        public.verify(Path(f"{tap}.sig").read_bytes(), tap.read_bytes())
        with zipfile.ZipFile(tap) as archive:
            public.verify(archive.read("app.app.elf.sig"), archive.read("app.app.elf"))
            package = json.loads(archive.read("package.json"))
        self.assertEqual(archive.namelist().count("app.app.elf"), 1)
        self.assertEqual(package["version"], "1.0.1")
        self.assertEqual(package["release_sequence"], 1_000_001)
        import tap_package
        self.assertEqual(tap_package.validate_package(tap)["release_sequence"], 1_000_001)
        # The key is never written, and nothing else lands among the assets.
        self.assertEqual(sorted(p.name for p in self.dist.iterdir()),
                         ["com.thistle.notes-1.0.1-esp32s3.tap",
                          "com.thistle.notes-1.0.1-esp32s3.tap.sig"])
        for path in self.root.rglob("*"):
            if path.is_file():
                self.assertNotIn(self.key_hex.encode(), path.read_bytes())

    def test_sign_without_a_key_fails_and_writes_nothing(self):
        self.stage()
        for key in (None, "", "zz" * 32, "ab" * 31):
            with self.subTest(key=key):
                result = self.sign(key)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("RELEASE_SIGNING_KEY", result.stderr)
                self.assertFalse(self.dist.exists() and any(self.dist.iterdir()))

    def test_the_catalog_entry_points_at_the_releases_assets(self):
        self.stage()
        self.assertEqual(self.sign(self.key_hex).returncode, 0)
        entry = self.entry()
        tap = (self.dist / "com.thistle.notes-1.0.1-esp32s3.tap").read_bytes()
        base = f"https://github.com/{REPO}/releases/download/notes-v1.0.1"
        self.assertEqual(entry["type"], "app")
        self.assertEqual(entry["id"], "com.thistle.notes")
        self.assertEqual(entry["version"], "1.0.1")
        self.assertEqual(entry["release_sequence"], 1_000_001)
        self.assertEqual(entry["arch"], "esp32s3")
        self.assertEqual(entry["min_os_version"], "0.6.0")
        self.assertEqual(entry["package_url"], f"{base}/com.thistle.notes-1.0.1-esp32s3.tap")
        self.assertEqual(entry["package_sig_url"], f"{base}/com.thistle.notes-1.0.1-esp32s3.tap.sig")
        self.assertEqual(entry["package_sha256"], hashlib.sha256(tap).hexdigest())
        self.assertEqual(entry["package_size_bytes"], len(tap))
        self.assertEqual(entry["changelog"], "- Saves on every keystroke\n- Fixed the cursor\n")
        self.assertEqual(entry["updated"], "2026-10-26")
        self.assertEqual(entry["publisher_key_id"], self.public.hex()[:16])
        self.assertTrue(entry["is_signed"])
        self.assertEqual(entry["permissions"], "storage")
        self.assertNotIn("url", entry)
        # Flat: the device's parser ends an entry at its first closing brace.
        self.assertNotIn("{", json.dumps(entry)[1:])

    def test_check_writes_the_steps_outputs(self):
        releases = self.root / "releases.json"
        # gh api --paginate --slurp writes a list of pages.
        releases.write_text(json.dumps([[firmware_release("v0.6.0")], []]))
        output = self.root / "github_output"
        env = dict(os.environ, GITHUB_OUTPUT=str(output))
        result = run_tool("check", "--tag", "notes-v1.0.1", "--app-dir", str(self.app),
                          "--releases", str(releases), env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_text(),
                         "app=notes\nversion=1.0.1\nprerelease=false\n")

    def test_check_refuses_with_the_reason(self):
        releases = self.root / "releases.json"
        releases.write_text(json.dumps([firmware_release("v0.5.2")]))
        env = dict(os.environ, GITHUB_OUTPUT=str(self.root / "github_output"))
        result = run_tool("check", "--tag", "notes-v1.0.1", "--app-dir", str(self.app),
                          "--releases", str(releases), env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("min_os 0.6.0 is newer than the newest firmware release, 0.5.2",
                      result.stderr)

    def test_the_store_apps_in_the_repository_stage(self):
        # Each of the four apps' own files make a valid release of their
        # current version, so a tag for it would get past `stage`.
        repo = Path(__file__).resolve().parent.parent
        for app in ra.APPS:
            with self.subTest(app=app):
                manifest = json.loads((repo / "standalone_apps" / app / "manifest.json").read_text())
                tag = f"{app}-v{manifest['version']}"
                ra.stage(tag, repo / "standalone_apps" / app, self.root / f"staged-{app}")


if __name__ == "__main__":
    unittest.main()
