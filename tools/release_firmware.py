#!/usr/bin/env python3
"""The firmware release's own steps, as .github/workflows/release-firmware.yml
runs them on a tag `vX.Y.Z` or `vX.Y.Z-suffix` (ADR-0011):

  tag    writes the tag's version and whether it is a prerelease to the
         step's outputs ($GITHUB_OUTPUT).
  stage  copies the built image and the full flash image into the release's
         folder under their asset names, writes the release's flat catalog
         entry, and the notes: CHANGELOG.md's section for the tag. No key.
  sign   writes the image's canonical V1 artifact manifest and signs it with
         the release key, read from the environment variable
         RELEASE_SIGNING_KEY (64 hex digits of the Ed25519 seed). The key is
         never written anywhere.
  sums   writes SHA256SUMS over every asset.

Tested by test_release_firmware.py.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sign import canonical_manifest, Ed25519PrivateKey  # noqa: E402

FIRMWARE_ID = "thistle-os"
KEY_VARIABLE = "RELEASE_SIGNING_KEY"

_NUMBER = r"(?:0|[1-9][0-9]*)"
# Prerelease identifiers are letters and digits, a number with no leading
# zero: a hyphen would read as git describe's `-<commits>-g<hash>`, which the
# kernel drops (version.rs).
_IDENTIFIER = r"(?:[0-9]*[A-Za-z][0-9A-Za-z]*|0|[1-9][0-9]*)"
TAG = re.compile(rf"^v({_NUMBER}\.{_NUMBER}\.{_NUMBER})(?:-({_IDENTIFIER}(?:\.{_IDENTIFIER})*))?$")


@dataclass(frozen=True)
class Release:
    tag: str
    version: str
    prerelease: bool


@dataclass(frozen=True)
class AssetNames:
    image: str
    manifest: str
    manifest_sig: str
    full_image: str
    catalog_entry: str


def parse_tag(tag):
    """`v0.6.0` is a release, `v0.6.0-rc.1` a prerelease; anything else is
    not a firmware tag."""
    match = TAG.match(tag)
    if not match:
        raise ValueError(f"{tag!r} is not a firmware tag (vX.Y.Z or vX.Y.Z-suffix)")
    return Release(tag=tag, version=tag[1:], prerelease=match.group(2) is not None)


VERSION = re.compile(rf"^({_NUMBER})\.({_NUMBER})\.({_NUMBER})(?:-({_IDENTIFIER}(?:\.{_IDENTIFIER})*))?$")


def version_key(version):
    """A sort key that orders versions as semver does, and as the kernel's
    version.rs: `0.6.0-rc.1` < `0.6.0-rc.2` < `0.6.0` < `0.10.0`."""
    match = VERSION.match(version)
    if not match:
        raise ValueError(f"{version!r} is not a version (X.Y.Z or X.Y.Z-suffix)")
    numbers = tuple(int(match.group(i)) for i in (1, 2, 3))
    if match.group(4) is None:
        return numbers + (1, ())
    identifiers = tuple((0, int(part), "") if part.isdigit() else (1, 0, part)
                        for part in match.group(4).split("."))
    return numbers + (0, identifiers)


def changelog_notes(changelog, tag):
    """The body of CHANGELOG.md's `## <tag>` or `## <tag> - <date>` section,
    up to the next `## ` heading."""
    heading = re.compile(rf"^## {re.escape(tag)}(?: - .*)?$")
    lines = changelog.splitlines()
    for index, line in enumerate(lines):
        if heading.match(line.rstrip()):
            body = []
            for following in lines[index + 1:]:
                if following.startswith("## "):
                    break
                body.append(following)
            text = "\n".join(body).strip()
            if not text:
                raise LookupError(f"CHANGELOG.md's section for {tag} is empty")
            return text + "\n"
    raise LookupError(f"CHANGELOG.md has no section for {tag} ('## {tag} - YYYY-MM-DD')")


def security_version(version_rs):
    """The number raised by hand in components/kernel_rs/src/version.rs."""
    match = re.search(r"pub const SECURITY_VERSION: u32 = ([0-9]+);", version_rs)
    if not match:
        raise ValueError("SECURITY_VERSION not found in version.rs")
    return int(match.group(1))


def compatible_boards(boards_dir, arch):
    """The board ids of every board profile for the chip."""
    boards = []
    for path in sorted(Path(boards_dir).glob("*.json")):
        board = json.loads(path.read_text()).get("board", {})
        if board.get("arch") == arch and board.get("board_id"):
            boards.append(board["board_id"])
    return sorted(set(boards))


def asset_names(tag, arch):
    image = f"thistle_os-{arch}-{tag}.bin"
    return AssetNames(
        image=image,
        manifest=f"{image}.manifest",
        manifest_sig=f"{image}.manifest.sig",
        full_image=f"thistle_os-{arch}-{tag}-full.bin",
        catalog_entry=f"thistle_os-{arch}-{tag}.catalog-entry.json",
    )


def download_url(repo, tag, name):
    return f"https://github.com/{repo}/releases/download/{tag}/{name}"


def catalog_entry(*, repo, tag, arch, boards, image, notes, updated):
    """The release's entry in the catalog, flat: the device's parser ends an
    entry at its first closing brace."""
    release = parse_tag(tag)
    names = asset_names(tag, arch)
    return {
        "type": "firmware",
        "id": FIRMWARE_ID,
        "version": release.version,
        "arch": arch,
        "compatible_boards": sorted(set(boards)),
        "url": download_url(repo, tag, names.image),
        "manifest_url": download_url(repo, tag, names.manifest),
        "manifest_sig_url": download_url(repo, tag, names.manifest_sig),
        "sha256": hashlib.sha256(image).hexdigest(),
        "size_bytes": len(image),
        "changelog": notes,
        "updated": updated,
    }


def release_key():
    """The release key from the environment, or SystemExit. Never printed."""
    key_hex = os.environ.get(KEY_VARIABLE, "")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", key_hex):
        raise SystemExit(f"error: {KEY_VARIABLE} must hold 64 hex digits (the Ed25519 seed)")
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(key_hex))


def cmd_tag(args):
    release = parse_tag(args.tag)
    outputs = (f"version={release.version}\n"
               f"prerelease={'true' if release.prerelease else 'false'}\n")
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(outputs)
    print(outputs, end="")


def cmd_stage(args):
    release = parse_tag(args.tag)
    names = asset_names(release.tag, args.arch)
    try:
        notes = changelog_notes(Path(args.changelog).read_text(), release.tag)
    except LookupError as error:
        raise SystemExit(f"error: {error}")
    dist = Path(args.dist)
    dist.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.image, dist / names.image)
    shutil.copyfile(args.full_image, dist / names.full_image)
    entry = catalog_entry(
        repo=args.repo, tag=release.tag, arch=args.arch,
        boards=compatible_boards(args.boards, args.arch),
        image=(dist / names.image).read_bytes(), notes=notes, updated=args.updated)
    (dist / names.catalog_entry).write_text(json.dumps(entry, indent=2) + "\n")
    Path(args.notes).write_text(notes)
    print(f"Staged {release.tag} ({'prerelease' if release.prerelease else 'release'}) in {dist}")


def cmd_sign(args):
    release = parse_tag(args.tag)
    names = asset_names(release.tag, args.arch)
    dist = Path(args.dist)
    key = release_key()
    manifest = canonical_manifest(
        artifact_type="firmware",
        artifact_id=FIRMWARE_ID,
        version=release.version,
        security_version=security_version(Path(args.version_rs).read_text()),
        arch=args.arch,
        compatible_boards=compatible_boards(args.boards, args.arch),
        url=download_url(args.repo, release.tag, names.image),
        payload=(dist / names.image).read_bytes(),
    )
    (dist / names.manifest).write_bytes(manifest)
    (dist / names.manifest_sig).write_bytes(key.sign(manifest))
    print(f"Signed {names.manifest}")


def cmd_sums(args):
    dist = Path(args.dist)
    lines = []
    for path in sorted(p for p in dist.iterdir() if p.is_file() and p.name != "SHA256SUMS"):
        lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    (dist / "SHA256SUMS").write_text("".join(lines))
    print(f"SHA256SUMS lists {len(lines)} assets")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def release_arguments(command):
        command.add_argument("--tag", required=True)
        command.add_argument("--repo", required=True, help="owner/name")
        command.add_argument("--arch", required=True)
        command.add_argument("--boards", required=True, help="board profiles folder")
        command.add_argument("--dist", required=True, help="the release's assets folder")

    tag = commands.add_parser("tag")
    tag.add_argument("tag")

    stage = commands.add_parser("stage")
    release_arguments(stage)
    stage.add_argument("--image", required=True)
    stage.add_argument("--full-image", required=True)
    stage.add_argument("--changelog", required=True)
    stage.add_argument("--updated", required=True, help="YYYY-MM-DD")
    stage.add_argument("--notes", required=True, help="where to write the notes")

    sign = commands.add_parser("sign")
    release_arguments(sign)
    sign.add_argument("--version-rs", required=True)

    sums = commands.add_parser("sums")
    sums.add_argument("--dist", required=True)

    args = parser.parse_args()
    try:
        commands = {"tag": cmd_tag, "stage": cmd_stage, "sign": cmd_sign, "sums": cmd_sums}
        commands[args.command](args)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"error: {error}")


if __name__ == "__main__":
    main()
