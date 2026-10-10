#!/usr/bin/env python3
"""A tap on an LVGL button opens it the first time, however short the tap.

On the T-Deck Max a tap used to turn a button black without opening it, and
the next tap opened it. Two faults in the UI manager's pointer bridge did it:
a press and release that both arrived between two pointer reads reached LVGL
as a release followed by a press that never ended, and every release went
back to reporting PRESSED one read later. Either way LVGL was left holding a
press nobody made, which blacked out the button under it and swallowed the
next tap.

--tap-hold 0 delivers the press and the release before LVGL next reads the
pointer, which is what a short tap looks like to a UI task that is busy.
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
    taps: tuple[str, ...]
    hold_ms: int
    expected: str


# Dock and grid positions are the launcher's own layout on each screen.
CASES = (
    Case("eink-quick-tap", "tdeck-max", ("27,294",), 0,
         "(settings) on_create"),
    Case("lcd-quick-tap", "tdeck", ("85,215",), 0,
         "(settings) on_create"),
    # The grid button, then a tile elsewhere: the second tap must not be
    # lost to a press left over from the first.
    Case("eink-second-tap-elsewhere", "tdeck-max", ("120,294", "120,95"), 100,
         "(filemgr) on_create"),
    Case("eink-quick-second-tap-elsewhere", "tdeck-max", ("120,294", "120,95"), 0,
         "(filemgr) on_create"),
)


def run_case(simulator: Path, case: Case) -> None:
    command = [str(simulator), "--headless", "--device", case.device,
               "--timeout", str(2500 + 1500 * len(case.taps)),
               "--tap-hold", str(case.hold_ms)]
    for tap in case.taps:
        command.extend(("--tap", tap))
    run = subprocess.run(command, text=True, capture_output=True,
                         check=False, timeout=60)
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    if case.expected not in run.stdout + run.stderr:
        raise AssertionError(f"no {case.expected!r} after the taps")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Touch tap regressions ===")
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
        print(f"{failures}/{len(CASES)} touch tap tests failed")
        return 1
    print(f"All {len(CASES)} touch tap tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
