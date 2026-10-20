#!/usr/bin/env python3
"""Build apps from standalone_apps/ into a folder to copy onto an SD card,
from which the App Store installs them with no network.

The folder mirrors the card's root:

    config/appstore.json          points the App Store at the catalog below
    appstore/catalog.json         the catalog as a Refresh keeps it, so the
                                  App Store lists the apps when it opens
    appstore/packages/catalog.json
    appstore/packages/<app>/<id>-<version>-<arch>.tap, and its .sig

Every catalog URL is file:///sdcard/..., which the kernel reads from the card
(appstore_client.rs, card_file()). Each ELF and package is signed with this
machine's own publisher key, tools/local_publisher_private.key, made on the
first run; git ignores it and its public half. A firmware built afterwards
trusts that key for apps (kernel_rs/CMakeLists.txt, kernel_boot.rs), so build
and flash once after the key is made.

Run with ESP-IDF's environment, for its cross compilers:

    . ~/esp/v5.5.1/esp-idf/export.sh
    python3 tools/make_card_appstore.py            # notes flashlight weather wifiscanner
    python3 tools/make_card_appstore.py --apps notes --out /media/$USER/SDCARD
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import date
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import build_catalog  # noqa: E402
import tap_package  # noqa: E402

PRIVATE_KEY = TOOLS / "local_publisher_private.key"
PUBLIC_KEY = TOOLS / "local_publisher_public.key"
DEFAULT_APPS = ("notes", "flashlight", "weather", "wifiscanner")
CARD_PACKAGES = "appstore/packages"


def publisher_key() -> tuple[Ed25519PrivateKey, bool]:
    """This machine's publisher key, made if there is none, kept as the raw
    32-byte seed. True if new. (cryptography, not PyNaCl, which ESP-IDF's
    Python environment lacks.)"""
    if PRIVATE_KEY.exists():
        key = Ed25519PrivateKey.from_private_bytes(PRIVATE_KEY.read_bytes())
        made = False
    else:
        key = Ed25519PrivateKey.generate()
        PRIVATE_KEY.touch(mode=0o600)
        PRIVATE_KEY.write_bytes(key.private_bytes(serialization.Encoding.Raw,
                                                  serialization.PrivateFormat.Raw,
                                                  serialization.NoEncryption()))
        made = True
    public = key.public_key().public_bytes_raw()
    if not PUBLIC_KEY.exists() or PUBLIC_KEY.read_bytes() != public:
        PUBLIC_KEY.write_bytes(public)
    return key, made


def compiler_for(arch: str) -> str:
    return "riscv32-esp-elf-gcc" if arch in ("esp32c3", "esp32c6") else f"xtensa-{arch}-elf-gcc"


def build_app(app: str, arch: str, idf: Path, work: Path) -> Path:
    source = REPO / "standalone_apps" / app
    manifest = json.loads((source / "manifest.json").read_text())
    build = work / f"build-{app}"
    for command in (
        ["cmake", "-S", str(source), "-B", str(build),
         f"-DCMAKE_TOOLCHAIN_FILE={idf}/tools/cmake/toolchain-{arch}.cmake"],
        ["cmake", "--build", str(build), "--parallel"],
    ):
        run = subprocess.run(command, text=True, capture_output=True, check=False)
        if run.returncode:
            sys.exit(f"building {app} failed:\n{run.stdout}\n{run.stderr}")
    elf = build / manifest["entry"]
    if not elf.is_file():
        sys.exit(f"{app} did not build {manifest['entry']}")
    return elf


def package_app(app: str, arch: str, elf: Path, key: Ed25519PrivateKey, packages: Path) -> Path:
    """The app's TAP package and its signature, beside its manifest, as
    build_catalog.scan_artifacts() expects them."""
    source = REPO / "standalone_apps" / app
    manifest = json.loads((source / "manifest.json").read_text())
    manifest["arch"] = arch
    destination = packages / app
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    with tempfile.TemporaryDirectory() as scratch:
        signature = Path(scratch) / f"{elf.name}.sig"
        signature.write_bytes(key.sign(elf.read_bytes()))
        tap = destination / f"{manifest['id']}-{manifest['version']}-{arch}.tap"
        tap_package.build_package(source / "manifest.json", source / "metadata.json", elf,
                                  signature, REPO / "LICENSE", tap, arch)
    Path(f"{tap}.sig").write_bytes(key.sign(tap.read_bytes()))
    return tap


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apps", nargs="+", default=list(DEFAULT_APPS),
                        help="folders of standalone_apps/ to build (default: %(default)s)")
    parser.add_argument("--arch", default="esp32s3", choices=sorted(tap_package.ARCHES - {"host"}))
    parser.add_argument("--out", type=Path, default=REPO / "card-appstore",
                        help="the folder to write, the card's root (default: %(default)s)")
    args = parser.parse_args()

    idf = os.environ.get("IDF_PATH")
    if not idf or not shutil.which(compiler_for(args.arch)):
        sys.exit(f"{compiler_for(args.arch)} is not on PATH: run "
                 ". ~/esp/v5.5.1/esp-idf/export.sh first")
    for app in args.apps:
        if not (REPO / "standalone_apps" / app / "manifest.json").is_file():
            sys.exit(f"no app standalone_apps/{app}")

    key, made = publisher_key()
    out = args.out.resolve()
    packages = out / CARD_PACKAGES
    if packages.exists():
        shutil.rmtree(packages)
    with tempfile.TemporaryDirectory(prefix="thistle-card-appstore-") as work:
        for app in args.apps:
            elf = build_app(app, args.arch, Path(idf), Path(work))
            tap = package_app(app, args.arch, elf, key, packages)
            print(f"  {tap.relative_to(out)}  ({tap.stat().st_size} bytes)")

    public_hex = key.public_key().public_bytes_raw().hex()
    entries = build_catalog.scan_artifacts(
        str(packages), f"file:///sdcard/{CARD_PACKAGES}",
        require_signatures=True, publisher_key_id=public_hex[:16])
    today = date.today().isoformat()
    for entry in entries:
        entry["updated"] = today
    catalog = json.dumps({"version": 1, "generated": today, "entries": entries}, indent=2) + "\n"
    (packages / "catalog.json").write_text(catalog)
    (out / "appstore" / "catalog.json").write_text(catalog)
    (out / "config").mkdir(parents=True, exist_ok=True)
    (out / "config" / "appstore.json").write_text(json.dumps(
        {"catalog_url": f"file:///sdcard/{CARD_PACKAGES}/catalog.json"}, indent=4) + "\n")

    print(f"\n{len(entries)} apps in {out}")
    print("Copy what is in it onto the SD card's root, then open the App Store.")
    print(f"Signed by this machine's publisher key {public_hex[:8]}...", end=" ")
    if made:
        print("(new: build and flash the firmware so it trusts the key)")
    else:
        print("(a firmware built here since it was made trusts it)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
