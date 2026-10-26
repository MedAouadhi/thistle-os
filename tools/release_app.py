#!/usr/bin/env python3
"""An app release's own steps, as .github/workflows/release-app.yml runs them
on a tag `<app>-vX.Y.Z` or `<app>-vX.Y.Z-suffix`, where <app> is one of the
App Store's apps in standalone_apps/ (ADR-0011):

  check  refuses the tag unless its version is the package manifest's and
         the app's min_os is no newer than the newest firmware release,
         prereleases included, in a Releases API listing. Writes the app, its
         version and whether it is a prerelease to the step's outputs.
  stage  writes the package manifest and metadata with the release sequence
         computed from the version, and the notes: the app's metadata for
         that version. No key.
  sign   signs the built ELF, builds the TAP package and signs it with the
         release key, read from the environment variable RELEASE_SIGNING_KEY
         (64 hex digits of the Ed25519 seed). The key is never written.
  entry  writes the release's catalog entry, beside the TAP.

SHA256SUMS is release_firmware.py's `sums`. Tested by test_release_app.py.
"""

import argparse
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_catalog  # noqa: E402
import release_firmware  # noqa: E402
import tap_package  # noqa: E402
from release_firmware import download_url, release_key, version_key  # noqa: E402

# The App Store's apps (CLAUDE.md, "App Store"). The samples hello and
# terminal, and the drivers, are not released.
APPS = ("notes", "weather", "wifiscanner", "flashlight")

_NUMBER = r"(?:0|[1-9][0-9]*)"
# As release_firmware.py's: a hyphen in a prerelease would read as git
# describe's `-<commits>-g<hash>`.
_IDENTIFIER = r"(?:[0-9]*[A-Za-z][0-9A-Za-z]*|0|[1-9][0-9]*)"
TAG = re.compile(
    rf"^([a-z0-9_]+)-v({_NUMBER}\.{_NUMBER}\.{_NUMBER}"
    rf"(?:-({_IDENTIFIER}(?:\.{_IDENTIFIER})*))?)$")


@dataclass(frozen=True)
class AppRelease:
    tag: str
    app: str
    version: str
    prerelease: bool


def parse_tag(tag):
    """`notes-v1.0.1` releases Notes 1.0.1; `notes-v1.0.1-rc.1` is a
    prerelease. An app outside the App Store, or anything else, is refused."""
    match = TAG.match(tag)
    if not match:
        raise ValueError(f"{tag!r} is not an app tag (<app>-vX.Y.Z or <app>-vX.Y.Z-suffix)")
    app = match.group(1)
    if app not in APPS:
        raise ValueError(f"{app!r} is not one of the App Store's apps ({', '.join(APPS)})")
    return AppRelease(tag=tag, app=app, version=match.group(2),
                      prerelease=match.group(3) is not None)


def released_firmware_versions(releases):
    """The versions of the firmware releases in a Releases API listing,
    prereleases included, drafts not."""
    versions = []
    for release in releases:
        if release.get("draft"):
            continue
        try:
            versions.append(release_firmware.parse_tag(release.get("tag_name", "")).version)
        except ValueError:
            continue  # an app's release, or a tag of another kind
    return versions


def newest_firmware(releases):
    """The newest firmware release's version, by version, or None."""
    versions = released_firmware_versions(releases)
    return max(versions, key=version_key) if versions else None


def read_json(path):
    return json.loads(Path(path).read_text())


def check(tag, app_dir, releases):
    """The tag's release, or ValueError saying why it is refused: its version
    is not the package manifest's, or its min_os is newer than the newest
    firmware release (prereleases included: an app may follow a release
    candidate, and devices hide what they cannot run). An empty min_os is
    any firmware's."""
    release = parse_tag(tag)
    manifest = read_json(Path(app_dir) / "manifest.json")
    if manifest.get("version") != release.version:
        raise ValueError(f"{tag} names version {release.version}, but "
                         f"{release.app}'s manifest.json says {manifest.get('version')}")
    min_os = manifest.get("min_os") or ""
    if not min_os:
        return release  # any firmware runs it: nothing to check against
    newest = newest_firmware(releases)
    if newest is None:
        raise ValueError(f"there is no firmware release yet for {release.app}'s "
                         f"min_os {min_os} to be checked against: release the firmware first")
    if version_key(min_os) > version_key(newest):
        raise ValueError(f"{release.app}'s min_os {min_os} is newer than the newest "
                         f"firmware release, {newest}: no device could install it")
    return release


def release_sequence(version):
    """major x 10^6 + minor x 10^3 + patch: the sequence a device compares
    to tell a newer release of an app (spec, "Versions")."""
    major, minor, patch = version_key(version)[:3]
    if minor >= 1000 or patch >= 1000:
        raise ValueError(f"{version}: minor and patch must be below 1000 to make a release sequence")
    return major * 1_000_000 + minor * 1_000 + patch


def release_notes(metadata, version):
    """The app's metadata's changes for the version, as a Markdown list."""
    releases = metadata.get("releases") or []
    entry = next((r for r in releases if r.get("version") == version), None)
    if entry is None:
        raise ValueError(f"metadata.json has no release {version}: add it, newest first, with its changes")
    if releases[0] is not entry:
        raise ValueError(f"metadata.json's newest release is {releases[0].get('version')}, not {version}")
    changes = entry.get("changes") or []
    if not changes:
        raise ValueError(f"metadata.json's release {version} lists no changes")
    return "".join(f"- {change}\n" for change in changes)


def stage(tag, app_dir, staged_dir):
    """Write the package manifest and metadata into staged_dir, each with the
    release sequence the tag's version gives, and return the notes. The
    repository's own files are left as they are."""
    release = parse_tag(tag)
    app_dir = Path(app_dir)
    manifest = read_json(app_dir / "manifest.json")
    metadata = read_json(app_dir / "metadata.json")
    notes = release_notes(metadata, release.version)
    sequence = release_sequence(release.version)
    manifest["release_sequence"] = sequence
    metadata["releases"][0]["release_sequence"] = sequence
    staged_dir = Path(staged_dir)
    staged_dir.mkdir(parents=True, exist_ok=True)
    (staged_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (staged_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return notes


def asset_names(manifest):
    """The TAP, its signature and the catalog entry, named as the package
    format names a TAP: <id>-<version>-<arch>."""
    stem = f"{manifest['id']}-{manifest['version']}-{manifest['arch']}"
    return {"tap": f"{stem}.tap", "tap_sig": f"{stem}.tap.sig",
            "catalog_entry": f"{stem}.catalog-entry.json"}


def cmd_check(args):
    release = check(args.tag, args.app_dir, build_catalog.load_releases(args.releases))
    outputs = (f"app={release.app}\n"
               f"version={release.version}\n"
               f"prerelease={'true' if release.prerelease else 'false'}\n")
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(outputs)
    print(f"{release.tag}: {release.app} {release.version}"
          f"{' (prerelease)' if release.prerelease else ''} may be released")


def cmd_stage(args):
    notes = stage(args.tag, args.app_dir, args.staged)
    Path(args.notes).write_text(notes)
    print(f"Staged {args.tag} with release sequence "
          f"{release_sequence(parse_tag(args.tag).version)}")


def cmd_sign(args):
    parse_tag(args.tag)
    key = release_key()
    staged = Path(args.staged)
    manifest = read_json(staged / "manifest.json")
    names = asset_names(manifest)
    dist = Path(args.dist)
    dist.mkdir(parents=True, exist_ok=True)
    tap = dist / names["tap"]
    with tempfile.TemporaryDirectory() as scratch:
        signature = Path(scratch) / "app.app.elf.sig"
        signature.write_bytes(key.sign(Path(args.elf).read_bytes()))
        tap_package.build_package(staged / "manifest.json", staged / "metadata.json",
                                  Path(args.elf), signature, Path(args.license), tap)
    (dist / names["tap_sig"]).write_bytes(key.sign(tap.read_bytes()))
    print(f"Signed {names['tap']}")


def cmd_entry(args):
    release = parse_tag(args.tag)
    staged = Path(args.staged)
    manifest = read_json(staged / "manifest.json")
    names = asset_names(manifest)
    tap = Path(args.dist) / names["tap"]
    entry = build_catalog.app_entry(
        tap_package.validate_package(tap), tap.read_bytes(),
        package_url=download_url(args.repo, release.tag, names["tap"]),
        package_sig_url=download_url(args.repo, release.tag, names["tap_sig"]),
        category=manifest.get("category", "tools"),
        publisher_key_id=Path(args.pubkey).read_bytes().hex()[:16],
        changelog=Path(args.notes).read_text(),
        updated=args.updated,
    )
    (Path(args.dist) / names["catalog_entry"]).write_text(json.dumps(entry, indent=2) + "\n")
    print(f"Wrote {names['catalog_entry']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    check_command = commands.add_parser("check")
    check_command.add_argument("--tag", required=True)
    check_command.add_argument("--app-dir", required=True, help="standalone_apps/<app>")
    check_command.add_argument("--releases", required=True, help="a Releases API listing (JSON)")

    stage_command = commands.add_parser("stage")
    stage_command.add_argument("--tag", required=True)
    stage_command.add_argument("--app-dir", required=True, help="standalone_apps/<app>")
    stage_command.add_argument("--staged", required=True, help="where to write the package's documents")
    stage_command.add_argument("--notes", required=True, help="where to write the notes")

    sign_command = commands.add_parser("sign")
    sign_command.add_argument("--tag", required=True)
    sign_command.add_argument("--staged", required=True)
    sign_command.add_argument("--elf", required=True, help="the built .app.elf")
    sign_command.add_argument("--license", required=True)
    sign_command.add_argument("--dist", required=True, help="the release's assets folder")

    entry_command = commands.add_parser("entry")
    entry_command.add_argument("--tag", required=True)
    entry_command.add_argument("--repo", required=True, help="owner/name")
    entry_command.add_argument("--staged", required=True)
    entry_command.add_argument("--dist", required=True)
    entry_command.add_argument("--notes", required=True)
    entry_command.add_argument("--updated", required=True, help="YYYY-MM-DD")
    entry_command.add_argument("--pubkey", required=True,
                               help="the release key's public half (32 raw bytes)")

    args = parser.parse_args()
    handlers = {"check": cmd_check, "stage": cmd_stage, "sign": cmd_sign, "entry": cmd_entry}
    try:
        handlers[args.command](args)
    except (OSError, ValueError, KeyError, tap_package.TapError) as error:
        raise SystemExit(f"error: {error}")


if __name__ == "__main__":
    main()
