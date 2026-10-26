#!/usr/bin/env python3
"""Regression tests for standalone package catalog publication."""

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from build_catalog import (catalog_from_releases, load_releases,
                           merge_catalog_entries, scan_artifacts)
from tap_package import build_package


class BuildCatalogTests(unittest.TestCase):
    def write_package(self, root: Path, kind: str, package_id: str) -> tuple[Path, bytes]:
        package_dir = root / "esp32s3" / f"{kind}s" / package_id
        package_dir.mkdir(parents=True)
        suffix = ".app.elf" if kind == "app" else ".drv.elf"
        elf_name = f"{package_id}{suffix}"
        elf_data = f"ELF:{package_id}".encode()
        (package_dir / elf_name).write_bytes(elf_data)
        (package_dir / f"{elf_name}.sig").write_bytes(b"s" * 64)
        manifest = {
            "id": f"com.thistle.{package_id}",
            "type": kind,
            "name": package_id,
            "version": "1.0.0",
            "release_sequence": 1,
            "author": "ThistleOS",
            "description": f"{package_id} package",
            "min_os": "0.3.0",
            "arch": "esp32s3",
            "entry": elf_name,
            "permissions": [],
            "background": False,
            "min_memory_kb": 16,
        }
        (package_dir / "manifest.json").write_text(json.dumps(manifest))
        if kind == "app":
            metadata = package_dir / "metadata.json"
            metadata.write_text(json.dumps({
                "schema": "thistle.app.metadata/v1", "id": manifest["id"],
                "category": "tools", "summary": f"{package_id} app",
                "description": f"{package_id} app", "license": "BSD-3-Clause",
                "releases": [{"version": "1.0.0", "release_sequence": 1,
                              "released": "2026-08-29", "changes": ["Initial"]}],
            }))
            license_path = package_dir / "LICENSE.source"
            license_path.write_text("BSD-3-Clause\n")
            tap = package_dir / f"{manifest['id']}-1.0.0-esp32s3.tap"
            build_package(
                package_dir / "manifest.json", metadata,
                package_dir / elf_name, package_dir / f"{elf_name}.sig",
                license_path, tap,
            )
            Path(f"{tap}.sig").write_bytes(b"t" * 64)
        return package_dir, elf_data

    def test_signed_apps_and_drivers_are_catalogued_with_qualified_urls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, app_data = self.write_package(root, "app", "hello")
            _, driver_data = self.write_package(root, "driver", "keyboard")

            entries = scan_artifacts(str(root), "https://example.test/apps",
                                     require_signatures=True)

            self.assertEqual({entry["type"] for entry in entries}, {"app", "driver"})
            by_id = {entry["id"]: entry for entry in entries}
            self.assertNotIn("url", by_id["com.thistle.hello"])
            self.assertNotIn("sha256", by_id["com.thistle.hello"])
            self.assertTrue(by_id["com.thistle.hello"]["package_url"].endswith(".tap"))
            self.assertEqual(by_id["com.thistle.keyboard"]["sha256"],
                             hashlib.sha256(driver_data).hexdigest())
            for entry in entries:
                self.assertTrue(entry["is_signed"])
                delivery_url = entry.get("package_url", entry.get("url"))
                self.assertIn("/esp32s3/", delivery_url)
                relative_url = delivery_url.removeprefix("https://example.test/apps/")
                self.assertTrue((root / relative_url).is_file())
                self.assertTrue((root / f"{relative_url}.sig").is_file())

    def test_missing_declared_elf_fails_catalog_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package_dir = root / "esp32s3" / "drivers" / "missing"
            package_dir.mkdir(parents=True)
            (package_dir / "manifest.json").write_text(json.dumps({
                "id": "com.thistle.missing",
                "type": "driver",
                "entry": "missing.drv.elf",
            }))

            with self.assertRaises(FileNotFoundError):
                scan_artifacts(str(root), "https://example.test/apps")

    def test_apps_publish_only_tap_delivery_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.write_package(root, "app", "notes")

            entries = scan_artifacts(
                str(root), "https://example.test/apps",
                require_signatures=True,
                publisher_key_id="official-2026",
            )
            entry = entries[0]
            self.assertNotIn("url", entry)
            self.assertNotIn("sig_url", entry)
            self.assertNotIn("sha256", entry)
            self.assertNotIn("size_bytes", entry)
            self.assertTrue(entry["package_url"].endswith(".tap"))
            self.assertEqual(entry["package_sig_url"], f'{entry["package_url"]}.sig')
            self.assertEqual(entry["publisher_key_id"], "official-2026")
            self.assertEqual(entry["release_sequence"], 1)

    def test_unsigned_package_fails_when_signatures_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package_dir, _ = self.write_package(root, "driver", "keyboard")
            (package_dir / "keyboard.drv.elf.sig").unlink()

            with self.assertRaises(FileNotFoundError):
                scan_artifacts(str(root), "https://example.test/apps",
                               require_signatures=True)

    def test_merge_drops_unrebuilt_apps_but_preserves_other_components(self):
        new_entries = [{"id": "com.thistle.hello", "type": "app", "rating": 0,
                        "rating_count": 0, "downloads": 0}]
        existing_entries = [
            {"id": "com.thistle.hello", "rating": 4.5,
             "rating_count": 12, "downloads": 99},
            {"id": "com.thistle.legacy", "type": "app",
             "url": "https://example.test/legacy.app.elf"},
            {"id": "com.thistle.keyboard", "type": "driver",
             "url": "https://example.test/keyboard.drv.elf"},
        ]

        merged = merge_catalog_entries(new_entries, existing_entries)

        by_id = {entry["id"]: entry for entry in merged}
        self.assertEqual(set(by_id), {"com.thistle.hello", "com.thistle.keyboard"})
        self.assertEqual(by_id["com.thistle.hello"]["rating"], 4.5)
        self.assertEqual(by_id["com.thistle.hello"]["downloads"], 99)
        self.assertNotIn("com.thistle.legacy", by_id)
        self.assertEqual(by_id["com.thistle.keyboard"]["url"],
                         "https://example.test/keyboard.drv.elf")


TESTDATA = Path(__file__).resolve().parent / "testdata"


class CatalogFromReleasesTests(unittest.TestCase):
    """The catalog Pages serves at store/catalog.json, derived from a
    recorded Releases API listing (two pages, as `gh api --paginate --slurp`
    writes them) and each release's catalog-entry asset."""

    def setUp(self):
        self.listing = load_releases(TESTDATA / "releases_api_listing.json")
        self.assets = json.loads((TESTDATA / "releases_api_catalog_entries.json").read_text())
        self.fetched = []

    def fetch(self, asset):
        self.fetched.append(asset["name"])
        return self.assets[asset["browser_download_url"]]

    def build(self, listing=None):
        return catalog_from_releases(self.listing if listing is None else listing,
                                     self.fetch, generated="2026-12-21")

    def entries(self, kind):
        return [entry for entry in self.build()["entries"] if entry["type"] == kind]

    def test_the_catalog_has_the_shape_the_device_parses(self):
        catalog = self.build()
        self.assertEqual(catalog["version"], 1)
        self.assertEqual(catalog["generated"], "2026-12-21")
        self.assertIsInstance(catalog["entries"], list)

    def test_the_newest_firmware_by_version_not_by_date(self):
        # v0.9.1 was published after v0.10.0; v0.11.0-rc.1 is a prerelease
        # and v0.12.0 a draft.
        self.assertEqual([e["version"] for e in self.entries("firmware")], ["0.10.0"])

    def test_the_newest_release_of_each_app(self):
        apps = {e["id"]: e["version"] for e in self.entries("app")}
        # 1.0.10 is newer than 1.0.9, though 1.0.9 was published later; the
        # 1.1.0 release candidate is a prerelease.
        self.assertEqual(apps, {"com.thistle.notes": "1.0.10", "com.thistle.weather": "1.0.1"})

    def test_drafts_and_prereleases_are_never_read(self):
        self.build()
        for name in self.fetched:
            with self.subTest(asset=name):
                self.assertNotIn("v0.12.0", name)
                self.assertNotIn("rc.1", name)

    def test_a_release_without_a_catalog_entry_is_passed_over(self):
        # v0.99.0 is upstream's kind of release, with no entry among its assets.
        self.assertNotIn("0.99.0", [e["version"] for e in self.entries("firmware")])

    def test_firmware_entries_are_flat(self):
        firmware = self.entries("firmware")[0]
        self.assertEqual(set(firmware), {
            "type", "id", "version", "arch", "compatible_boards", "url", "manifest_url",
            "manifest_sig_url", "sha256", "size_bytes", "changelog", "updated"})
        for value in firmware.values():
            self.assertNotIsInstance(value, dict)

    def test_an_entry_that_disagrees_with_its_tag_is_refused(self):
        url = next(u for u in self.assets if "/v0.10.0/" in u)
        self.assets[url] = dict(self.assets[url], version="0.10.1")
        with self.assertRaisesRegex(ValueError, "v0.10.0"):
            self.build()

    def test_a_firmware_entry_that_is_not_flat_is_refused(self):
        url = next(u for u in self.assets if "/v0.10.0/" in u)
        self.assets[url] = dict(self.assets[url], extra={"nested": True})
        with self.assertRaisesRegex(ValueError, "flat"):
            self.build()

    def test_no_releases_make_an_empty_catalog(self):
        self.assertEqual(self.build([])["entries"], [])

    def test_the_command_line_writes_store_catalog(self):
        with tempfile.TemporaryDirectory() as tmp:
            # Entries fetched from file:// URLs stand in for GitHub's.
            listing = json.loads((TESTDATA / "releases_api_listing.json").read_text())
            for page in listing:
                for release in page:
                    for asset in release["assets"]:
                        entry = self.assets.get(asset["browser_download_url"])
                        if entry is not None:
                            path = Path(tmp) / f"{release['tag_name']}-{asset['name']}"
                            path.write_text(json.dumps(entry))
                            asset["browser_download_url"] = path.as_uri()
            listing_path = Path(tmp) / "releases.json"
            listing_path.write_text(json.dumps(listing))
            output = Path(tmp) / "_site" / "store" / "catalog.json"
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parent / "build_catalog.py"),
                 "--releases", str(listing_path), "--output", str(output)],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            catalog = json.loads(output.read_text())
            self.assertEqual(sorted((e["type"], e["version"]) for e in catalog["entries"]),
                             [("app", "1.0.1"), ("app", "1.0.10"), ("firmware", "0.10.0")])


if __name__ == "__main__":
    unittest.main()
