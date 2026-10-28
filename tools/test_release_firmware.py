#!/usr/bin/env python3
"""The firmware release's own steps (tools/release_firmware.py): what a tag
says, the notes CHANGELOG.md gives it, the release's assets, its artifact
manifest signed with a key from the environment, its catalog entry and
SHA256SUMS. Run from the repository root."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_firmware as rf  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import (  # noqa: E402
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

TOOL = Path(__file__).resolve().parent / "release_firmware.py"
REPO = "MedAouadhi/thistle-os"

CHANGELOG = """# ThistleOS Changelog

## v0.6.0 - 2026-11-02

Over-the-air updates.

- Kernel slots.

### Details

- More.

## v0.6.0-rc.1 - 2026-10-30

First release candidate.

- Tried by USB.

## v0.5.2 - 2026-05-03

Recovery trust-root hardening release.

## Unreleased

- Work in progress.
"""


class TagTests(unittest.TestCase):
    def test_a_plain_tag_is_a_full_release(self):
        release = rf.parse_tag("v0.6.0")
        self.assertEqual(release.version, "0.6.0")
        self.assertFalse(release.prerelease)

    def test_a_suffixed_tag_is_a_prerelease(self):
        release = rf.parse_tag("v0.6.0-rc.1")
        self.assertEqual(release.version, "0.6.0-rc.1")
        self.assertTrue(release.prerelease)

    def test_the_tag_command_writes_step_outputs(self):
        for tag, expected in [("v0.6.0-rc.1", "version=0.6.0-rc.1\nprerelease=true\n"),
                              ("v0.6.0", "version=0.6.0\nprerelease=false\n")]:
            with self.subTest(tag=tag), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "out"
                output.write_text("")
                result = subprocess.run(
                    [sys.executable, str(TOOL), "tag", tag],
                    env=dict(os.environ, GITHUB_OUTPUT=str(output)),
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output.read_text(), expected)
        refused = subprocess.run([sys.executable, str(TOOL), "tag", "notes-v1.0.0"],
                                 capture_output=True, text=True)
        self.assertNotEqual(refused.returncode, 0)

    def test_other_tags_are_refused(self):
        for tag in ["0.6.0", "v0.6", "v0.6.0.1", "notes-v1.0.1", "v0.6.0-3-gabc1234",
                    "v0.6.0-", "v0.6.0-rc..1", "v0.6.0+build", "v06.0.0", "vx.y.z"]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                rf.parse_tag(tag)


class NotesTests(unittest.TestCase):
    def test_the_tags_section_is_the_notes(self):
        notes = rf.changelog_notes(CHANGELOG, "v0.6.0-rc.1")
        self.assertEqual(notes, "First release candidate.\n\n- Tried by USB.\n")

    def test_subsections_belong_to_their_section(self):
        notes = rf.changelog_notes(CHANGELOG, "v0.6.0")
        self.assertIn("### Details\n\n- More.", notes)
        self.assertNotIn("First release candidate", notes)

    def test_a_heading_without_a_date(self):
        notes = rf.changelog_notes("## v0.7.0\n\nThings.\n", "v0.7.0")
        self.assertEqual(notes, "Things.\n")

    def test_a_tag_is_not_matched_by_its_prefix(self):
        # v0.6.0's section is not v0.6.0-rc.1's, nor the other way round.
        with self.assertRaises(LookupError):
            rf.changelog_notes("## v0.6.0-rc.1\n\nCandidate.\n", "v0.6.0")

    def test_no_section_refuses_the_release(self):
        with self.assertRaisesRegex(LookupError, "CHANGELOG.md has no section for v0.9.0"):
            rf.changelog_notes(CHANGELOG, "v0.9.0")

    def test_an_empty_section_refuses_the_release(self):
        with self.assertRaises(LookupError):
            rf.changelog_notes("## v0.7.0\n\n## v0.6.0\n\nOld.\n", "v0.7.0")


class SourceTests(unittest.TestCase):
    def test_the_security_version_is_read_from_the_source(self):
        text = "pub const SECURITY_VERSION: u32 = 512;\n"
        self.assertEqual(rf.security_version(text), 512)
        with self.assertRaises(ValueError):
            rf.security_version("no such constant")

    def test_the_repositorys_own_security_version(self):
        text = Path("components/kernel_rs/src/version.rs").read_text()
        self.assertGreater(rf.security_version(text), 0)

    def test_compatible_boards_are_the_chips_boards(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, arch in [("tdeck-max", "esp32s3"), ("tdeck", "esp32s3"),
                               ("c3-mini", "esp32c3")]:
                (root / f"{name}.json").write_text(json.dumps(
                    {"board": {"board_id": name, "arch": arch}}))
            self.assertEqual(rf.compatible_boards(root, "esp32s3"), ["tdeck", "tdeck-max"])

    def test_the_repositorys_boards_include_the_tdeck_max(self):
        boards = rf.compatible_boards(Path("sdcard_layout/config/boards"), "esp32s3")
        self.assertIn("tdeck-max", boards)
        self.assertNotIn("c3-mini", boards)


class AssetTests(unittest.TestCase):
    def test_asset_names_carry_the_chip_and_the_tag(self):
        names = rf.asset_names("v0.6.0-rc.1", "esp32s3")
        self.assertEqual(names.image, "thistle_os-esp32s3-v0.6.0-rc.1.bin")
        self.assertEqual(names.manifest, "thistle_os-esp32s3-v0.6.0-rc.1.bin.manifest")
        self.assertEqual(names.manifest_sig, "thistle_os-esp32s3-v0.6.0-rc.1.bin.manifest.sig")
        self.assertEqual(names.full_image, "thistle_os-esp32s3-v0.6.0-rc.1-full.bin")
        self.assertEqual(names.catalog_entry, "thistle_os-esp32s3-v0.6.0-rc.1.catalog-entry.json")

    def test_the_catalog_entry_is_flat(self):
        image = b"kernel image bytes"
        entry = rf.catalog_entry(repo=REPO, tag="v0.6.0", arch="esp32s3",
                                 boards=["tdeck-max", "tdeck"], image=image,
                                 notes="Notes.\n", updated="2026-11-02")
        base = f"https://github.com/{REPO}/releases/download/v0.6.0/"
        self.assertEqual(entry, {
            "type": "firmware",
            "id": "thistle-os",
            "version": "0.6.0",
            "arch": "esp32s3",
            "compatible_boards": ["tdeck", "tdeck-max"],
            "url": base + "thistle_os-esp32s3-v0.6.0.bin",
            "manifest_url": base + "thistle_os-esp32s3-v0.6.0.bin.manifest",
            "manifest_sig_url": base + "thistle_os-esp32s3-v0.6.0.bin.manifest.sig",
            "sha256": hashlib.sha256(image).hexdigest(),
            "size_bytes": len(image),
            "changelog": "Notes.\n",
            "updated": "2026-11-02",
        })
        # The device's parser ends an entry at its first closing brace.
        self.assertFalse(any(isinstance(v, dict) for v in entry.values()))


class StageSignAndSumTests(unittest.TestCase):
    """The CLI as the workflow runs it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        build = self.root / "build"
        build.mkdir()
        self.image = b"\xe9 a kernel image"
        (build / "thistle_os.bin").write_bytes(self.image)
        self.full = self.root / "full.bin"
        self.full.write_bytes(b"bootloader + partition table + kernel")
        boards = self.root / "boards"
        boards.mkdir()
        (boards / "tdeck-max.json").write_text(
            json.dumps({"board": {"board_id": "tdeck-max", "arch": "esp32s3"}}))
        (self.root / "CHANGELOG.md").write_text(CHANGELOG)
        (self.root / "version.rs").write_text("pub const SECURITY_VERSION: u32 = 501;\n")
        self.dist = self.root / "dist"
        self.tag = "v0.6.0-rc.1"
        self.names = rf.asset_names(self.tag, "esp32s3")
        self.key = Ed25519PrivateKey.generate()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args, env=None):
        environment = {k: v for k, v in os.environ.items() if k != "RELEASE_SIGNING_KEY"}
        environment.update(env or {})
        return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True,
                              text=True, env=environment)

    def stage(self):
        out = self.run_tool(
            "stage", "--tag", self.tag, "--repo", REPO, "--arch", "esp32s3",
            "--image", str(self.root / "build/thistle_os.bin"),
            "--full-image", str(self.full),
            "--boards", str(self.root / "boards"),
            "--changelog", str(self.root / "CHANGELOG.md"),
            "--updated", "2026-10-30", "--dist", str(self.dist),
            "--notes", str(self.root / "notes.md"))
        self.assertEqual(out.returncode, 0, out.stderr)

    def sign(self, key_hex):
        return self.run_tool(
            "sign", "--tag", self.tag, "--repo", REPO, "--arch", "esp32s3",
            "--boards", str(self.root / "boards"),
            "--version-rs", str(self.root / "version.rs"), "--dist", str(self.dist),
            env={"RELEASE_SIGNING_KEY": key_hex} if key_hex is not None else None)

    def test_stage_writes_the_image_full_image_entry_and_notes(self):
        self.stage()
        self.assertEqual((self.dist / self.names.image).read_bytes(), self.image)
        self.assertEqual((self.dist / self.names.full_image).read_bytes(),
                         self.full.read_bytes())
        entry = json.loads((self.dist / self.names.catalog_entry).read_text())
        self.assertEqual(entry["version"], "0.6.0-rc.1")
        self.assertEqual(entry["sha256"], hashlib.sha256(self.image).hexdigest())
        self.assertEqual(entry["changelog"], "First release candidate.\n\n- Tried by USB.\n")
        self.assertEqual((self.root / "notes.md").read_text(), entry["changelog"])

    def test_stage_refuses_a_tag_without_notes(self):
        self.tag = "v0.9.0"
        out = self.run_tool(
            "stage", "--tag", self.tag, "--repo", REPO, "--arch", "esp32s3",
            "--image", str(self.root / "build/thistle_os.bin"),
            "--full-image", str(self.full), "--boards", str(self.root / "boards"),
            "--changelog", str(self.root / "CHANGELOG.md"),
            "--updated", "2026-10-30", "--dist", str(self.dist),
            "--notes", str(self.root / "notes.md"))
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("no section for v0.9.0", out.stderr)

    def test_sign_writes_a_canonical_manifest_the_public_key_verifies(self):
        self.stage()
        out = self.sign(self.key.private_bytes_raw().hex())
        self.assertEqual(out.returncode, 0, out.stderr)
        manifest = (self.dist / self.names.manifest).read_bytes()
        signature = (self.dist / self.names.manifest_sig).read_bytes()
        self.key.public_key().verify(signature, manifest)
        base = f"https://github.com/{REPO}/releases/download/{self.tag}/"
        self.assertEqual(manifest.decode().splitlines(), [
            "THISTLE-ARTIFACT-MANIFEST-V1",
            "type=firmware",
            "id=thistle-os",
            "version=0.6.0-rc.1",
            "security_version=501",
            "arch=esp32s3",
            "compatible_boards=tdeck-max",
            "destination=kernel",
            f"sha256={hashlib.sha256(self.image).hexdigest()}",
            f"size={len(self.image)}",
            f"url={base}{self.names.image}",
        ])

    def test_sign_writes_nothing_of_the_key(self):
        self.stage()
        key_hex = self.key.private_bytes_raw().hex()
        out = self.sign(key_hex)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertNotIn(key_hex, out.stdout + out.stderr)
        for path in self.root.rglob("*"):
            if path.is_file():
                data = path.read_bytes()
                self.assertNotIn(self.key.private_bytes_raw(), data, path)
                self.assertNotIn(key_hex.encode(), data, path)

    def test_sign_without_a_key_fails(self):
        self.stage()
        for key in [None, "", "zz" * 32, "ab" * 31]:
            with self.subTest(key=key):
                out = self.sign(key)
                self.assertNotEqual(out.returncode, 0)
                self.assertFalse((self.dist / self.names.manifest_sig).exists())

    def test_the_committed_public_key_checks_the_manifest(self):
        # The workflow checks its own signature against tools/production_public.key
        # before publishing: a manifest signed by another key fails.
        self.stage()
        self.assertEqual(self.sign(self.key.private_bytes_raw().hex()).returncode, 0)
        committed = Path("tools/production_public.key").resolve()
        out = subprocess.run([sys.executable, "tools/sign.py", "verify",
                              str(self.dist / self.names.manifest), "--pubkey", str(committed)],
                             capture_output=True, text=True)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("FAIL", out.stdout)

    def test_sums_list_every_asset(self):
        self.stage()
        self.sign(self.key.private_bytes_raw().hex())
        out = self.run_tool("sums", "--dist", str(self.dist))
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = (self.dist / "SHA256SUMS").read_text().splitlines()
        listed = {}
        for line in lines:
            digest, name = line.split("  ", 1)
            listed[name] = digest
        expected = {self.names.image, self.names.manifest, self.names.manifest_sig,
                    self.names.full_image, self.names.catalog_entry}
        self.assertEqual(set(listed), expected)
        for name, digest in listed.items():
            self.assertEqual(digest, hashlib.sha256((self.dist / name).read_bytes()).hexdigest())
        # `sha256sum -c` reads it as written.
        check = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=self.dist,
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)


class CommittedKeyTests(unittest.TestCase):
    def test_the_committed_public_key_is_a_key(self):
        raw = Path("tools/production_public.key").read_bytes()
        self.assertEqual(len(raw), 32)
        Ed25519PublicKey.from_public_bytes(raw)


if __name__ == "__main__":
    unittest.main()
