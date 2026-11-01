#!/usr/bin/env python3
"""Background checks run on a task of their own, hold the device awake, are
logged, and are skipped or stopped as their policy says.

The simulator registers a check named `sim` only when
THISTLE_SIM_BACKGROUND_CHECK="SECONDS[:OUTCOME]" is set
(background_check.rs, the `sim` module): every SECONDS it says which task it
runs on, how many tasks of that name are alive (1 once the last run's task is
gone) and what holds the device awake, then ends as OUTCOME says.

The simulator's board does not sleep, so a check woken from light sleep is
the power policy's host tests' and the device's to show.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

RUNNING = "[checks] sim check: running on task check (1 of that name), held awake by sim"


@dataclass(frozen=True)
class Case:
    name: str
    spec: Optional[str]
    timeout_ms: int
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    # (line, least, most) times it appears
    counts: tuple[tuple[str, int, int], ...] = ()


CASES = (
    Case("runs-on-a-task-of-its-own-held-awake-and-logged", "2", 7500,
         expected=("[checks] check sim registered: every 2 s (0: off), 16 KB of internal RAM",
                   RUNNING,
                   "[checks] check sim: done (nothing new) in 0.2 s; internal RAM ",
                   " KB free before, ",
                   RUNNING),
         # Due 2 s after registration, then 2 s after each run ends
         counts=((RUNNING, 2, 3),)),
    Case("a-failure-is-tried-again-an-interval-later", "2:failed", 7500,
         expected=("[checks] check sim: failed (the simulator said so) in 0.2 s",),
         counts=(("[checks] check sim: failed", 2, 3),)),
    Case("a-check-may-say-it-skipped", "2:skipped", 5000,
         expected=("[checks] check sim: skipped (the simulator said so) in 0.2 s",)),
    Case("stopped-until-a-time-it-does-not-run-again", "2:stopped", 8000,
         expected=("[checks] check sim: stopped for 60 min in 0.2 s",),
         counts=(("[checks] sim check: running", 1, 1),)),
    Case("too-little-internal-ram-skips-it-unrun", "2:lowram", 7000,
         expected=("[checks] check sim: skipped: ",
                   " KB of internal RAM free, "),
         absent=("[checks] sim check: running",),
         counts=(("[checks] check sim: skipped: ", 2, 3),)),
    Case("an-interval-of-0-is-off", "0", 5000,
         expected=("[checks] check sim registered: every 0 s (0: off)",),
         absent=("[checks] sim check: running",)),
    Case("no-check-without-the-variable", None, 5000,
         absent=("[checks] check sim registered",)),
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
    env = dict(os.environ)
    env.pop("THISTLE_SIM_BACKGROUND_CHECK", None)
    if case.spec is not None:
        env["THISTLE_SIM_BACKGROUND_CHECK"] = case.spec
    command = [str(simulator), "--headless", "--device", "tdeck-max",
               "--timeout", str(case.timeout_ms)]
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
    for line, least, most in case.counts:
        seen = log.count(line)
        if not least <= seen <= most:
            raise AssertionError(f"{line!r} {seen} times, not {least} to {most}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run the case of this name alone")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or c.name == args.only]
    print("=== Background checks ===")
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
        print(f"{failures}/{len(cases)} background check tests failed")
        return 1
    print(f"All {len(cases)} background check tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
