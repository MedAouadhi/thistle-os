#!/usr/bin/env python3
"""Every key typed reaches the focused text field, however fast it is typed.

The TCA8418 driver drains the keyboard's whole event FIFO in one poll, so a
key can go down and up, or several keys can, before LVGL next reads the
keypad. The UI manager's keypad bridge held one state, so LVGL saw only the
last of those events and the keys before it were lost. Fast typists also
press a key before letting go of the one before; LVGL tracks one key, so the
second read as the first still held and was never typed.

--type delivers every press and release before LVGL next reads the keypad,
and --type-rollover overlaps each key with the next. The simulator prints the
focused text field on exit.

Backspace held for 2 s empties the focused field, and a shorter hold does
not. LVGL repeats a held key, so the text is typed with the cursor sent back
to its start (LV_KEY_HOME, 0x02): there the repeat deletes nothing, and only
the hold can empty the field. The Z typed after the hold lands at the cursor.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

TEXT = "hello world"
HOLD_TEXT = "hold to clear"
CLEARED = "Backspace held: text field cleared"


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    rollover: bool


@dataclass(frozen=True)
class HoldCase:
    name: str
    device: str
    hold_ms: int
    expected: str


CASES = (
    Case("eink-burst", "tdeck-max", False),
    Case("lcd-burst", "tdeck", False),
    Case("eink-rollover", "tdeck-max", True),
    Case("lcd-rollover", "tdeck", True),
    HoldCase("eink-hold-clears", "tdeck-max", 2500, "Z"),
    HoldCase("lcd-hold-clears", "tdeck", 2500, "Z"),
    HoldCase("eink-short-hold-keeps", "tdeck-max", 1500, "Z" + HOLD_TEXT),
)


def run_case(simulator: Path, case: Case | HoldCase) -> None:
    command = [str(simulator), "--headless", "--device", case.device,
               "--launch-app", "com.thistle.terminal"]
    if isinstance(case, HoldCase):
        command += ["--timeout", "7500", "--tap", f"type:{HOLD_TEXT}\x02",
                    "--tap", f"backspace:{case.hold_ms}", "--type", "Z"]
        text = case.expected
    else:
        command += ["--timeout", "3500", "--type", TEXT]
        if case.rollover:
            command.append("--type-rollover")
        text = TEXT
    run = subprocess.run(command, text=True, capture_output=True,
                         check=False, timeout=60)
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    expected = f'Focused text: "{text}"'
    if expected not in run.stdout:
        got = [line for line in run.stdout.splitlines()
               if line.startswith("Focused text:")]
        raise AssertionError(f"expected {expected!r}, got {got}")
    if isinstance(case, HoldCase):
        cleared = run.stdout.count(CLEARED)
        if cleared != (1 if text == "Z" else 0):
            raise AssertionError(f"field cleared {cleared} times")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Keyboard typing regressions ===")
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
        print(f"{failures}/{len(CASES)} keyboard typing tests failed")
        return 1
    print(f"All {len(CASES)} keyboard typing tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
