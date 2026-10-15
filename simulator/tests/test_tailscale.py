#!/usr/bin/env python3
"""The tailnet joins by a sign-in URL, says why it was refused, rejoins by
itself once joined, and is switched from the quick panel and signed out of
from the app.

The simulator's client is a pretend one (tailnet_manager.rs, the host
`platform` module): THISTLE_SIM_TAILNET says how its control plane behaves,
and whether the device has joined is a file in the simulator's SPIFFS, which
each case gets fresh in a temporary directory.

Coordinates are the T-Deck Max's (240 x 320): the quick panel's third tile
is Tailscale, and the app's Sign out button sits right of Switch off once
connected.

The simulator's SD card is simulator/sdcard, which is in the repository, so
every case puts system.json back as it found it.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

SYSTEM_JSON = Path(__file__).resolve().parent.parent / "sdcard" / "config" / "system.json"

APP = "com.thistle.tailscale"
TAILSCALE_TILE = "198,52"
SIGN_OUT = "190,190"
JOINED = ".tailnet_joined"


def thistle_os(system: dict) -> dict:
    return system.get("thistle_os", system)


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    timeout_ms: int
    enabled: bool = True
    joined: bool = False
    env: tuple[tuple[str, str], ...] = ()
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    once: tuple[str, ...] = ()
    # Checked after the run, on the case's SPIFFS directory and system.json
    after: Optional[Callable[[Path, dict], Optional[str]]] = field(default=None)


def joined(spiffs: Path, _system: dict) -> Optional[str]:
    return None if (spiffs / JOINED).exists() else "the device did not join"


def switched_on(_spiffs: Path, system: dict) -> Optional[str]:
    on = thistle_os(system).get("tailnet", {}).get("enabled")
    return None if on is True else f"thistle_os.tailnet.enabled is {on!r}"


def signed_out(spiffs: Path, system: dict) -> Optional[str]:
    if (spiffs / JOINED).exists():
        return "the identity is still there"
    on = thistle_os(system).get("tailnet", {}).get("enabled")
    return None if on is False else f"thistle_os.tailnet.enabled is {on!r}"


CASES = (
    Case("signs-in-by-url", ("--launch-app", APP), 9000,
         env=(("THISTLE_SIM_TAILNET_LOGIN_S", "3"),),
         expected=("[tailnet] on", "(tailscale) on_create", "[tailnet] waiting for a sign-in",
                   "[tailnet] connected as thistle-sim, 100.64.0.1"),
         after=joined),
    Case("waits-for-an-admin-to-approve", ("--launch-app", APP), 9000,
         env=(("THISTLE_SIM_TAILNET", "approval"), ("THISTLE_SIM_TAILNET_LOGIN_S", "2")),
         expected=("[tailnet] waiting for a sign-in",
                   "[tailnet] waiting for an admin to approve this device",
                   "[tailnet] connected as")),
    Case("a-refusal-is-said-and-stops-it", ("--launch-app", APP), 6000,
         env=(("THISTLE_SIM_TAILNET", "refuse"),),
         expected=("[tailnet] stopped",
                   "[tailnet] The tailnet refused this device: node not allowed"),
         absent=("[tailnet] connected as",),
         # Not started again: a refusal waits for the switch
         once=("[tailnet] connecting",)),
    Case("rejoins-without-signing-in", (), 6000, joined=True,
         expected=("[tailnet] connected as",),
         absent=("[tailnet] waiting for a sign-in",)),
    Case("stays-off-until-switched-on", (), 5000, enabled=False, joined=True,
         absent=("[tailnet] connecting", "[tailnet] connected as")),
    Case("the-quick-panel-switches-it-on", ("--tap", "quick", "--tap", TAILSCALE_TILE), 8000,
         enabled=False, joined=True,
         expected=("(quick_panel) open", "[tailnet] switched on",
                   "(quick_panel) Tailscale -> on", "[tailnet] connected as"),
         after=switched_on),
    Case("sign-out-forgets-the-device",
         ("--launch-app", APP, "--tap", "wait", "--tap", "wait",
          "--tap", SIGN_OUT, "--tap", SIGN_OUT), 11000, joined=True,
         expected=("[tailnet] connected as", "[tailnet] this device's tailnet identity is erased"),
         after=signed_out),
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


def run_case(simulator: Path, case: Case, original: bytes) -> None:
    system = json.loads(original)
    thistle_os(system)["tailnet"] = {"enabled": case.enabled}
    SYSTEM_JSON.write_text(json.dumps(system, indent=2))

    with tempfile.TemporaryDirectory(prefix="thistle-tailnet-") as tmp:
        spiffs = Path(tmp)
        if case.joined:
            (spiffs / JOINED).write_text("1")
        env = dict(os.environ, THISTLE_SIM_SPIFFS=str(spiffs))
        env.pop("THISTLE_SIM_TAILNET", None)
        env.pop("THISTLE_SIM_TAILNET_PEERS", None)
        env.update(dict(case.env))

        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(case.timeout_ms), *case.args]
        run = subprocess.run(command, text=True, capture_output=True,
                             check=False, timeout=90, env=env)
        log = run.stdout + run.stderr
        if run.returncode != 0:
            raise AssertionError(f"simulator exited {run.returncode}")
        missing = in_order(log, case.expected)
        if missing:
            raise AssertionError(f"no {missing!r}, in order")
        for line in case.absent:
            if line in log:
                raise AssertionError(f"unexpected {line!r}")
        for line in case.once:
            if log.count(line) > 1:
                raise AssertionError(f"{line!r} {log.count(line)} times")
        if case.after:
            problem = case.after(spiffs, json.loads(SYSTEM_JSON.read_text()))
            if problem:
                raise AssertionError(problem)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run the case of this name alone")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or c.name == args.only]
    print("=== Tailscale ===")
    original = SYSTEM_JSON.read_bytes()
    failures = 0
    try:
        for case in cases:
            try:
                run_case(simulator, case, original)
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.name}: {exc}")
            else:
                print(f"PASS  {case.name}")
            finally:
                SYSTEM_JSON.write_bytes(original)
    finally:
        SYSTEM_JSON.write_bytes(original)

    if failures:
        print(f"{failures}/{len(cases)} Tailscale tests failed")
        return 1
    print(f"All {len(cases)} Tailscale tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
