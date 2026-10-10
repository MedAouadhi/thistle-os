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
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

TEXT = "hello world"


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    rollover: bool


CASES = (
    Case("eink-burst", "tdeck-max", False),
    Case("lcd-burst", "tdeck", False),
    Case("eink-rollover", "tdeck-max", True),
    Case("lcd-rollover", "tdeck", True),
)


def run_case(simulator: Path, case: Case) -> None:
    command = [str(simulator), "--headless", "--device", case.device,
               "--timeout", "3500", "--launch-app", "com.thistle.terminal",
               "--type", TEXT]
    if case.rollover:
        command.append("--type-rollover")
    run = subprocess.run(command, text=True, capture_output=True,
                         check=False, timeout=60)
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    expected = f'Focused text: "{TEXT}"'
    if expected not in run.stdout:
        got = [line for line in run.stdout.splitlines()
               if line.startswith("Focused text:")]
        raise AssertionError(f"expected {expected!r}, got {got}")


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
