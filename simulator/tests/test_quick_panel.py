#!/usr/bin/env python3
"""The quick panel opens on a drag down from the status bar, or the quick key,
switches what its tiles say, saves the choice, and closes every way it should.

Coordinates are the panel's own layout: on the T-Deck Max (240 x 320) it hangs
from y 24, with tiles 72 px wide and 44 high from y 30, the volume slider's
row centred on y 184, and the Settings button at the bottom left.

The simulator's SD card is simulator/sdcard, which is in the repository, so
every case puts system.json back as it found it.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

SYSTEM_JSON = Path(__file__).resolve().parent.parent / "sdcard" / "config" / "system.json"

WIFI_TILE = "42,52"
# The third row, which only dictation's tile is on.
DICTATION_TILE = "42,152"
VOLUME_SLIDER = "200,236"
SETTINGS_BUTTON = "50,271"
BELOW_PANEL = "120,300"


def thistle_os(system: dict) -> dict:
    return system.get("thistle_os", system)


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    args: tuple[str, ...]
    steps: int
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    saved: Optional[Callable[[dict], bool]] = field(default=None)


CASES = (
    Case("eink-drag-down-opens", "tdeck-max", ("--drag", "120,8,120,160"), 1,
         expected=("(quick_panel) open",)),
    Case("lcd-drag-down-opens", "tdeck", ("--drag", "160,8,160,140"), 1,
         expected=("(quick_panel) open",)),
    Case("drag-from-below-the-bar-does-not-open", "tdeck-max",
         ("--drag", "120,60,120,220"), 1,
         absent=("(quick_panel) open",)),
    Case("quick-key-opens-and-closes", "tdeck-max",
         ("--tap", "quick", "--tap", "quick"), 2,
         expected=("(quick_panel) open", "(quick_panel) closed")),
    Case("a-tile-switches-and-is-saved", "tdeck-max",
         ("--tap", "quick", "--tap", WIFI_TILE), 2,
         expected=("(quick_panel) WiFi -> off",),
         saved=lambda s: thistle_os(s).get("wifi", {}).get("radio_off") is True),
    Case("the-dictation-tile-picks-english", "tdeck-max",
         ("--tap", "quick", "--tap", DICTATION_TILE), 2,
         expected=("(quick_panel) Dictation -> on",),
         saved=lambda s: thistle_os(s).get("dictation", {}).get("language") == "en"),
    Case("a-slider-sets-and-saves-the-volume", "tdeck-max",
         ("--tap", "quick", "--tap", VOLUME_SLIDER), 2,
         expected=("(quick_panel) Volume -> ",),
         saved=lambda s: isinstance(thistle_os(s).get("audio", {}).get("volume"), int)),
    Case("drag-up-closes", "tdeck-max",
         ("--tap", "quick", "--drag", "120,250,120,90"), 2,
         expected=("(quick_panel) open", "(quick_panel) closed")),
    Case("tap-below-closes", "tdeck-max",
         ("--tap", "quick", "--tap", BELOW_PANEL), 2,
         expected=("(quick_panel) open", "(quick_panel) closed")),
    Case("back-closes-and-stays-in-the-app", "tdeck-max",
         ("--launch-app", "com.thistle.notes", "--tap", "quick", "--tap", "back"), 2,
         expected=("(notes) on_create", "(quick_panel) open", "(quick_panel) closed"),
         absent=("(notes) on_destroy",)),
    Case("settings-button-opens-settings", "tdeck-max",
         ("--tap", "quick", "--tap", SETTINGS_BUTTON), 2,
         expected=("(quick_panel) closed", "(settings) on_create")),
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


def run_case(simulator: Path, case: Case) -> None:
    command = [str(simulator), "--headless", "--device", case.device,
               "--timeout", str(2500 + 1500 * case.steps), *case.args]
    run = subprocess.run(command, text=True, capture_output=True,
                         check=False, timeout=60)
    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    if case.saved and not case.saved(json.loads(SYSTEM_JSON.read_text())):
        raise AssertionError("the choice did not reach system.json")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Quick panel ===")
    original = SYSTEM_JSON.read_bytes()
    failures = 0
    try:
        for case in CASES:
            try:
                run_case(simulator, case)
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
        print(f"{failures}/{len(CASES)} quick panel tests failed")
        return 1
    print(f"All {len(CASES)} quick panel tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
