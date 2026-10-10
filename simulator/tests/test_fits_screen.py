#!/usr/bin/env python3
"""Screens reached by tapping fit the display, edge to edge.

On a T-Deck Max the WiFi password dialog lost the first letter of "Connect to"
and "Password" and the outer borders of both its buttons: it was 280 px wide,
centred on a 240 px panel, so 20 px hung off each side. Toasts were the same
280 px. test_app_matrix.py checks every app's first screen; these are screens
only taps reach, so it never sees them.

The simulator's WiFi scan finds a fixed list whose first network has a long
name and a password, so the first row opens the dialog with the hardest title.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    app: str
    taps: tuple[str, ...]
    expected: str


# Settings: the WiFi row, Scan Networks, then the first network. The rows sit
# at the same height on every screen, so one set of taps serves all three.
PASSWORD_DIALOG = ("120,69", "120,100", "120,131")
# WiFi Scanner on a 240 px screen: Scan, the first network, then Connect,
# which toasts that a secured network needs Settings.
SCANNER_TOAST = ("162,38", "120,68", "46,218")

CASES = (
    Case("eink-max-password-dialog", "tdeck-max", "com.thistle.settings",
         PASSWORD_DIALOG, "attempting connect to"),
    Case("eink-pro-password-dialog", "tdeck-pro", "com.thistle.settings",
         PASSWORD_DIALOG, "attempting connect to"),
    Case("lcd-password-dialog", "tdeck", "com.thistle.settings",
         PASSWORD_DIALOG, "attempting connect to"),
    Case("eink-max-toast", "tdeck-max", "com.thistle.wifiscanner",
         SCANNER_TOAST, "toast: Use Settings"),
)


def run_case(simulator: Path, case: Case) -> None:
    # Exit one tap gap after the last tap: long enough for its screen to
    # settle, short enough that a toast is still up.
    command = [str(simulator), "--headless", "--device", case.device,
               "--launch-app", case.app, "--report-offscreen",
               "--timeout", str(2250 + 1500 * len(case.taps))]
    for tap in case.taps:
        command.extend(("--tap", tap))
    run = subprocess.run(command, text=True, capture_output=True,
                         check=False, timeout=60)
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    if case.expected not in run.stdout + run.stderr:
        raise AssertionError(f"no {case.expected!r}: the taps missed")
    if "Off-screen objects: " not in run.stdout:
        raise AssertionError("simulator did not report off-screen objects")
    offscreen = [line for line in run.stdout.splitlines()
                 if line.startswith("Off screen: ")]
    if offscreen:
        raise AssertionError("wider than the screen: " + "; ".join(offscreen))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Screens fit the display ===")
    failures = 0
    for case in CASES:
        try:
            run_case(simulator, case)
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(CASES)} fit cases failed")
        return 1
    print(f"All {len(CASES)} fit cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
