#!/usr/bin/env python3
"""The LVGL App Store on the simulated T-Deck Max: its lists survive a card
full of apps, the Installed tab lists and removes what the boot scan loads,
Refresh keeps the server's catalog whole on the card, and an install runs on
the App Store's worker task and fetches the package the kept catalog names.

A local HTTP server stands in for the catalog host; the card's
config/appstore.json points the App Store at it. The simulator's kernel
trusts only the production signing key, so an install here stops at the
package's signature: what the cases check is that the right files were
fetched, off lvgl_task, and how the App Store said it went.

Coordinates are the app's own layout on the T-Deck Max (240 x 320, the
status bar 24 px; appstore_ui.c): the tabs are on the title bar, y 24 to 54,
Installed at its right end; each list's rows are two lines high, the first
y 54 to 93, sorted by name on the Installed tab; the Catalog tab's Refresh is
the right of the two buttons above its status line.

A catalog can also be on the card, with its packages, as
tools/make_card_appstore.py writes one: file:///sdcard/... URLs, which the
kernel reads from the card with no server asked.

Every case runs on a scratch copy of simulator/sdcard.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable, Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"

TAB_INSTALLED = "197,38"
FIRST_ROW = "100,75"
SECOND_ROW = "100,114"
REFRESH = "178,286"
INSTALL = "57,272"            # on an entry's details, below a two-line description
REMOVE_INSTALLED = "57,197"   # on an installed app's details

PACKAGE = bytes(range(256)) * 16
SIGNATURE = bytes(64)


def catalog(base: str) -> bytes:
    return json.dumps({
        "version": 1,
        "entries": [
            {
                "id": "com.thistle.notes", "type": "app", "name": "Notes",
                "version": "1.2.0", "author": "ThistleOS", "arch": "host",
                "description": "Write and organise plain-text notes on the card",
                "release_sequence": 3,
                "package_url": f"{base}/notes.tap",
                "package_sig_url": f"{base}/notes.tap.sig",
                "package_sha256": hashlib.sha256(PACKAGE).hexdigest(),
                "package_size_bytes": len(PACKAGE),
                "permissions": "storage",
            },
            {
                "id": "com.thistle.elsewhere", "type": "app", "name": "Elsewhere",
                "version": "1.0.0", "arch": "esp32c6",
                "description": "Built for another chip",
            },
            {
                "id": "thistle-os", "type": "firmware", "name": "ThistleOS",
                "version": "0.4.0", "description": "A newer ThistleOS",
            },
        ],
    }, indent=2).encode()


class Host:
    """The catalog host: what it serves, and every path asked of it."""

    def __init__(self, catalog_status: int = 200) -> None:
        self.catalog_status = catalog_status
        self.requests: list[str] = []
        host = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # noqa: N802
                host.requests.append(self.path)
                body, status = host.answer(self.path)
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.catalog = catalog(self.base)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def answer(self, path: str) -> tuple[bytes, int]:
        if path == "/catalog.json":
            return (self.catalog, 200) if self.catalog_status == 200 else (b"down", self.catalog_status)
        if path == "/notes.tap":
            return PACKAGE, 200
        if path == "/notes.tap.sig":
            return SIGNATURE, 200
        return b"", 404

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def elfs(count: int) -> Callable[[Path, Host], None]:
    """count ELFs copied onto the card, each with its signature."""
    def setup(card: Path, host: Host) -> None:
        for i in range(count):
            (card / "apps" / f"com.example.app{i:02d}.app.elf").write_bytes(bytes(2048))
            (card / "apps" / f"com.example.app{i:02d}.app.elf.sig").write_bytes(SIGNATURE)
    return setup


def installed_package(card: Path, host: Host) -> None:
    """Notes as the App Store installs it (tap_installer.rs)."""
    generation = card / "apps/com.thistle.notes/generations/3"
    generation.mkdir(parents=True)
    (generation / "app.app.elf").write_bytes(bytes(4096))
    (generation / "app.app.elf.sig").write_bytes(SIGNATURE)
    (generation / "package.json").write_text(json.dumps({
        "schema": "thistle.app.package/v1", "type": "app", "id": "com.thistle.notes",
        "name": "Notes", "version": "1.2.0", "release_sequence": 3, "entry": "app.app.elf",
    }))
    (card / "apps/com.thistle.notes/active.json").write_text(
        '{"release_sequence": 3, "version": "1.2.0"}')


def kept_catalog(card: Path, host: Host) -> None:
    """The catalog as an earlier Refresh kept it."""
    (card / "appstore/catalog.json").write_bytes(host.catalog)


CARD_PACKAGES = "file:///sdcard/appstore/packages"


def card_catalog(card: Path, host: Host) -> None:
    """The catalog and Notes' package on the card, the App Store pointed at
    them, as make_card_appstore.py leaves a card."""
    packages = card / "appstore/packages"
    packages.mkdir(parents=True)
    (packages / "catalog.json").write_bytes(catalog(CARD_PACKAGES))
    (packages / "notes.tap").write_bytes(PACKAGE)
    (packages / "notes.tap.sig").write_bytes(SIGNATURE)
    (card / "config/appstore.json").write_text(
        json.dumps({"catalog_url": f"{CARD_PACKAGES}/catalog.json"}))


def card_catalog_kept(card: Path, host: Host) -> None:
    card_catalog(card, host)
    (card / "appstore/catalog.json").write_bytes(catalog(CARD_PACKAGES))


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    setup: Optional[Callable[[Path, Host], None]] = None
    catalog_status: int = 200
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    # Paths the host must have been asked for, in order.
    fetched: tuple[str, ...] = ()
    # Checks on the card after the run.
    check: Optional[Callable[[Path, Host], Optional[str]]] = None
    timeout_ms: int = 5000


def catalog_kept_whole(card: Path, host: Host) -> Optional[str]:
    kept = card / "appstore/catalog.json"
    if not kept.exists():
        return "no appstore/catalog.json"
    if kept.read_bytes() != host.catalog:
        return "appstore/catalog.json is not what the server sent"
    return None


def catalog_untouched(card: Path, host: Host) -> Optional[str]:
    if (card / "appstore/catalog.json").read_bytes() == host.catalog:
        return "a failed refresh replaced the kept catalog"
    return None


def notes_removed(card: Path, host: Host) -> Optional[str]:
    if (card / "apps/com.thistle.notes").exists():
        return "apps/com.thistle.notes is still there"
    if not (card / "apps/com.example.app00.app.elf").exists():
        return "the other app went too"
    return None


def card_catalog_kept_whole(card: Path, host: Host) -> Optional[str]:
    if host.requests:
        return f"the server was asked for {host.requests}"
    kept = card / "appstore/catalog.json"
    if not kept.exists() or kept.read_bytes() != catalog(CARD_PACKAGES):
        return "appstore/catalog.json is not the card's catalog"
    return None


def nothing_installed_nor_asked(card: Path, host: Host) -> Optional[str]:
    if host.requests:
        return f"the server was asked for {host.requests}"
    return nothing_installed(card, host)


def nothing_installed(card: Path, host: Host) -> Optional[str]:
    if (card / "apps/com.thistle.notes").exists() and any((card / "apps/com.thistle.notes").rglob("*.elf")):
        return "an unverified package was unpacked"
    return None


CASES = (
    Case("a-card-full-of-apps-lists-without-running-the-pool-dry",
         ("--tap", TAB_INSTALLED), setup=elfs(40),
         expected=("Found 32 installed apps",)),
    Case("an-elf-and-its-signature-are-one-app",
         ("--tap", TAB_INSTALLED), setup=elfs(3),
         expected=("Found 3 installed apps",)),
    # The ELF, com.example.app00, comes first.
    Case("a-store-install-is-listed-and-removed-whole",
         ("--tap", TAB_INSTALLED, "--tap", SECOND_ROW, "--tap", REMOVE_INSTALLED),
         setup=lambda card, host: (installed_package(card, host), elfs(1)(card, host)),
         expected=("Found 2 installed apps", "Removed: com.thistle.notes",
                   "toast: Removed Notes. It leaves the launcher at the next restart.",
                   "Found 1 installed apps"),
         check=notes_removed, timeout_ms=6500),
    Case("refresh-keeps-the-servers-catalog-whole",
         ("--tap", REFRESH),
         expected=("Fetching catalog: ", "Parsed 2 catalog entries", "(appstore_job) done: ESP_OK",
                   "toast: Catalog updated"),
         fetched=("/catalog.json",), check=catalog_kept_whole),
    Case("a-failed-refresh-keeps-the-catalog-it-had",
         ("--tap", REFRESH), catalog_status=500,
         setup=lambda card, host: (card / "appstore/catalog.json").write_text("[]"),
         expected=("Catalog fetch failed: HTTP 500", "toast: Could not reach the catalog server"),
         fetched=("/catalog.json",), check=catalog_untouched),
    Case("install-fetches-the-package-the-kept-catalog-names",
         ("--tap", FIRST_ROW, "--tap", INSTALL),
         setup=kept_catalog,
         expected=("Loaded 2 entries from local cache", "(appstore_job) done: ESP_ERR_INVALID_CRC",
                   "toast: Notes: corrupted, or its signature is invalid"),
         absent=("no installable package",),
         fetched=("/notes.tap", "/notes.tap.sig"), check=nothing_installed, timeout_ms=6500),
    Case("a-catalog-on-the-card-needs-no-server",
         ("--tap", REFRESH), setup=card_catalog,
         expected=("Catalog read from the card: ", "Parsed 2 catalog entries",
                   "(appstore_job) done: ESP_OK", "toast: Catalog updated"),
         check=card_catalog_kept_whole),
    # The simulator trusts no publisher key, so the copy stops at the
    # signature, as a download does.
    Case("a-package-on-the-card-installs-from-it",
         ("--tap", FIRST_ROW, "--tap", INSTALL), setup=card_catalog_kept,
         expected=("Loaded 2 entries from local cache", "(appstore_job) done: ESP_ERR_INVALID_CRC",
                   "toast: Notes: corrupted, or its signature is invalid"),
         absent=("no installable package", "No network connection"),
         check=nothing_installed_nor_asked, timeout_ms=6500),
)


def in_order(log: str, expected: tuple[str, ...]) -> Optional[str]:
    """The first expected line missing, or out of order."""
    at = 0
    for line in expected:
        found = log.find(line, at)
        if found < 0:
            return line
        at = found + len(line)
    return None


def run_case(simulator: Path, case: Case, keep: Optional[Path]) -> None:
    host = Host(case.catalog_status)
    try:
        with tempfile.TemporaryDirectory(prefix="thistle-appstore-") as tmp:
            card = Path(tmp) / "card"
            shutil.copytree(SDCARD, card)
            shutil.rmtree(card / "apps")
            (card / "apps").mkdir()
            (card / "appstore").mkdir(exist_ok=True)
            (card / "appstore/catalog.json").unlink(missing_ok=True)
            (card / "config/appstore.json").write_text(
                json.dumps({"catalog_url": f"{host.base}/catalog.json"}))
            if case.setup:
                case.setup(card, host)
            env = dict(os.environ, THISTLE_SIM_SDCARD=str(card),
                       THISTLE_SIM_SPIFFS=str(Path(tmp) / "spiffs"))

            command = [str(simulator), "--headless", "--device", "tdeck-max",
                       "--launch-app", "com.thistle.appstore",
                       "--timeout", str(case.timeout_ms), *case.args]
            if keep:
                command += ["--screenshot", str(keep / f"appstore-{case.name}.ppm")]
            run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                                 check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
            problem = case.check(card, host) if case.check else None
    finally:
        host.close()

    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    if "App launched: com.thistle.appstore" not in log:
        raise AssertionError("the app did not launch")
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    asked = [path for path in host.requests if path in case.fetched]
    if tuple(asked) != case.fetched:
        raise AssertionError(f"the host was asked for {host.requests}, not {list(case.fetched)}")
    if problem:
        raise AssertionError(problem)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    parser.add_argument("--screenshots", type=Path, help="keep each case's last frame here")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=True)

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== App Store ===")
    failures = 0
    for case in cases:
        try:
            run_case(simulator, case, args.screenshots)
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(cases)} App Store tests failed")
        return 1
    print(f"All {len(cases)} App Store tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
