#!/usr/bin/env python3
"""Home's rows of what is new on the simulated T-Deck Max: new messages, a
missed call with its call-back button, the tailnet waiting for a sign-in,
updates to ThistleOS and the installed apps, and "All caught up" with nothing
new; each row's tap opens what it is about.

The update row reads what the kernel's update manager judged of the catalog
the last check or Refresh kept on the card (appstore/catalog.json): no
network is needed, as a restart reads that catalog back.

The launcher prints its rows whenever they change, as
"(launcher_ui) now: TITLE (DETAIL) | TITLE (DETAIL)", and a call back as
"(launcher_ui) call back NUMBER: ESP_OK".

The simulated modem (drivers/sim_cellular.c) fetches an --sms once it
registers, and rings with --call 3 s after that, giving up after 20 s, which
wanders with the boot by seconds; so the taps start from the registration
(registered+MS) or the ring (ring+MS), as test_calls.py's do. A missed call's
row shows once the call screen has said "Missed call" and closed, about
23 s after the ring.

Coordinates are Home's own layout on the T-Deck Max (240 x 320, the status
bar 24 px; home_core.c): the rows' band is y 88 to 168, and a row alone sits
in its middle, its call-back button at its right end.

Every case runs on a scratch copy of simulator/sdcard and an empty SPIFFS:
the call log (phone/calls.jsonl) and the message history are kept on the
card, and a missed call left there by one run would show on the next one's
Home.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"

NOW = "(launcher_ui) now: "
DOT = " • "
CALLER = "0701234567"
# In simulator/sdcard/contacts/contacts.vcf, as "+49 160 9876543".
CONTACT = "+491609876543"

# A row alone, and its call-back button.
ROW = "100,128"
CALL_BACK = "216,128"

REGISTERED = "registered+2500"
# The call screen says "Missed call" 20 s after the ring, for 2.5 s.
AFTER_MISSED = "ring+25000"


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    timeout_ms: int
    # Each a run of lines that must appear in this order. The UI, the
    # kernel's threads and the simulator's main loop print from threads of
    # their own, so a run keeps to one of them, or to lines a poll apart.
    expected: tuple[tuple[str, ...], ...] = ()
    absent: tuple[str, ...] = ()
    env: tuple[tuple[str, str], ...] = ()
    tailnet: bool = False
    # Puts what the case needs on the scratch card.
    card: Optional[Callable[[Path], None]] = None


NOTES = "com.thistle.notes"


def notes_installed(card: Path, version: str = "1.0.0", sequence: int = 1_000_000) -> None:
    """Notes as the App Store installs a package (tap_installer.rs)."""
    app = card / "apps" / NOTES
    generation = app / "generations" / str(sequence)
    generation.mkdir(parents=True)
    (generation / "app.app.elf").write_bytes(bytes(4096))
    (generation / "app.app.elf.sig").write_bytes(bytes(64))
    (generation / "package.json").write_text(json.dumps({
        "schema": "thistle.app.package/v1", "type": "app", "id": NOTES, "name": "Notes",
        "version": version, "release_sequence": sequence, "entry": "app.app.elf"}))
    (app / "active.json").write_text(json.dumps({"release_sequence": sequence, "version": version}))


def kept_catalog(firmware: Optional[str] = None, notes: Optional[tuple[str, int, str]] = None):
    """A catalog an earlier check kept on the card: a ThistleOS release for
    the simulator's chip and board, and a Notes release (version, release
    sequence, min_os); Notes 1.0.0 installed."""
    def setup(card: Path) -> None:
        entries = []
        if firmware:
            url = f"https://example.invalid/v{firmware}/thistle_os-host-v{firmware}.bin"
            entries.append({
                "type": "firmware", "id": "thistle-os", "version": firmware, "arch": "host",
                "compatible_boards": ["tdeck-max"], "url": url, "manifest_url": url + ".manifest",
                "manifest_sig_url": url + ".manifest.sig", "sha256": "00" * 32,
                "size_bytes": 8192, "changelog": f"What is new in {firmware}"})
        if notes:
            version, sequence, min_os = notes
            entries.append({
                "type": "app", "id": NOTES, "name": "Notes", "version": version, "arch": "host",
                "release_sequence": sequence, "min_os_version": min_os,
                "package_url": "https://example.invalid/notes.tap",
                "package_sig_url": "https://example.invalid/notes.tap.sig",
                "package_sha256": "00" * 32, "package_size_bytes": 4096})
        (card / "appstore" / "catalog.json").write_text(
            json.dumps({"version": 1, "entries": entries}, indent=2))
        notes_installed(card)
    return setup


CASES = (
    Case("quiet-home-is-all-caught-up",
         ("--tap", ROW), 5000,
         expected=((NOW + "All caught up (No new messages or calls)",),),
         # Not a button; and with no missed call the phonebook is left
         # unread, as its first use searches the whole card.
         absent=("Launching app from Home", "[phonebook]", NOW + "Mesh on")),
    Case("messages-row-counts-sms-and-mesh",
         ("--mesh-direct", "Anna: hi", "--sms", "Reza: hi"), 10000,
         expected=((NOW + "1 new message (From the mesh)",
                    NOW + "2 new messages (1 SMS" + DOT + "1 from the mesh)"),
                   # The status bar still has its pop-up: Home never takes it.
                   ("[sim_cellular] SMS fetched: 1", "toast: " "" " Reza: hi")),
         absent=("All caught up",)),
    Case("messages-row-opens-the-messenger",
         ("--sms", "Reza: hi", "--tap-start", REGISTERED, "--tap", ROW), 10000,
         expected=((NOW + "1 new message (SMS)",
                    "Launching app from Home: com.thistle.messenger",
                    "(messenger) on_create"),)),
    Case("missed-call-row-names-the-number",
         ("--call", CALLER), 34000,
         expected=(("(call_ui) ended shown: Missed call",
                    NOW + "Missed call (" + CALLER + DOT),
                   ("(call_ui) missed calls: 1",)),
         # The count is the Phone app's Recents' to clear.
         absent=("(call_ui) missed calls: 0",)),
    Case("missed-call-is-named-from-the-phonebook",
         ("--call", CONTACT), 34000,
         expected=((NOW + "Missed call (Ben Müller" + DOT,),)),
    Case("missed-call-button-calls-back",
         ("--call", CALLER, "--tap-start", AFTER_MISSED, "--tap", CALL_BACK), 34000,
         expected=((NOW + "Missed call (" + CALLER,
                    f"(launcher_ui) call back {CALLER}: ESP_OK",
                    "(call_ui) open outgoing"),
                   (f"[sim_cellular] dialing {CALLER}",)),
         absent=("Launching app from Home",)),
    Case("missed-call-row-opens-phone",
         ("--call", CALLER, "--tap-start", AFTER_MISSED, "--tap", ROW), 34000,
         expected=((NOW + "Missed call (" + CALLER,
                    "Launching app from Home: com.thistle.phone",
                    "(phone) on_create"),),
         absent=("call back",)),
    # Where the call-back button would be, the row itself is.
    Case("withheld-missed-call-has-no-call-back",
         ("--call", "", "--tap-start", AFTER_MISSED, "--tap", CALL_BACK), 34000,
         expected=((NOW + "Missed call (Withheld" + DOT,
                    "Launching app from Home: com.thistle.phone"),),
         absent=("call back", "[sim_cellular] dialing")),
    Case("tailscale-row-asks-for-a-sign-in-and-opens-tailscale",
         ("--tap-start", "5000", "--tap", ROW), 8000,
         env=(("THISTLE_SIM_TAILNET", "login"), ("THISTLE_SIM_TAILNET_LOGIN_S", "60")),
         tailnet=True,
         expected=(("[tailnet] waiting for a sign-in",),
                   (NOW + "Tailscale (Sign in to join your tailnet)",
                    "Launching app from Home: com.thistle.tailscale",
                    "(tailscale) on_create"),)),
    # Ticket 09 of .scratch/ota: ThistleOS on offer, and an app update.
    Case("update-row-names-thistleos-and-opens-system-update",
         ("--tap-start", "5000", "--tap", ROW), 9000,
         card=kept_catalog(firmware="99.0.0", notes=("1.0.1", 1_000_001, "0.1.0")),
         expected=((NOW + "System update (ThistleOS 99.0.0 and 1 app)",
                    "Launching app from Home: com.thistle.settings",
                    "Settings opens at System update",
                    "System update: Available: ThistleOS 99.0.0"),),
         absent=("All caught up",)),
    Case("app-updates-alone-open-the-app-store",
         ("--tap-start", "5000", "--tap", ROW), 9000,
         card=kept_catalog(notes=("1.0.1", 1_000_001, "0.1.0")),
         expected=((NOW + "App updates (1 app has an update)",
                    "Launching app from Home: com.thistle.appstore",
                    "(appstore) on_create"),),
         absent=("Settings opens at", "All caught up")),
    Case("an-app-release-that-needs-newer-firmware-is-not-counted",
         (), 5000,
         card=kept_catalog(notes=("1.0.1", 1_000_001, "99.0.0")),
         expected=((NOW + "All caught up (No new messages or calls)",),),
         absent=("App updates", "System update (")),
    Case("an-app-release-already-installed-is-not-counted",
         (), 5000,
         card=kept_catalog(notes=("1.0.0", 1_000_000, "0.1.0")),
         expected=((NOW + "All caught up (No new messages or calls)",),),
         absent=("App updates",)),
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


def switch_tailnet_on(card: Path) -> None:
    path = card / "config" / "system.json"
    system = json.loads(path.read_text())
    system.get("thistle_os", system)["tailnet"] = {"enabled": True}
    path.write_text(json.dumps(system, indent=2))


def run_case(simulator: Path, case: Case) -> None:
    with tempfile.TemporaryDirectory(prefix="thistle-home-") as tmp:
        card = Path(tmp) / "card"
        spiffs = Path(tmp) / "spiffs"
        shutil.copytree(SDCARD, card)
        spiffs.mkdir()
        if case.tailnet:
            switch_tailnet_on(card)
        if case.card:
            case.card(card)
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs))
        env.pop("THISTLE_SIM_TAILNET", None)
        env.pop("THISTLE_SIM_TAILNET_LOGIN_S", None)
        env.update(dict(case.env))

        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(case.timeout_ms), *case.args]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    for sequence in case.expected:
        missing = in_order(log, sequence)
        if missing:
            raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== Home ===")
    failures = 0
    for case in cases:
        try:
            run_case(simulator, case)
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(cases)} Home tests failed")
        return 1
    print(f"All {len(cases)} Home tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
