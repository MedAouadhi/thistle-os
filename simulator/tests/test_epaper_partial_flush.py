#!/usr/bin/env python3
"""A widget redrawn on its own looks the same on e-paper as it did at first.

On the T-Deck Max the WiFi and Bluetooth values in Settings ("Off >") turned
into slanted noise a couple of seconds after the screen opened, while the
names beside them stayed sharp. A timer rewrites those values every two
seconds, so LVGL flushes each one as a small area of its own, 31 pixels wide.
The LVGL e-paper flush packed 1-bit rows back to back; the panel drivers read
each row from a fresh byte. The two agree only when the width is a multiple of
8, which full-width flushes always are and a lone label usually is not.

The simulator's e-paper display now decodes rows the way the drivers do, so
the same shear shows up here. This captures Settings before the timer first
fires and again after it has fired, and requires the two to match below the
status bar, whose clock is allowed to move. Both runs' flush logs are checked
too, so a change in timing fails here instead of passing for nothing: the
first capture must hold only full-width flushes, and the second a flush whose
width is not a multiple of 8.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

STATUS_BAR_HEIGHT = 24
# --log-flushes prints every area the window manager hands the display.
FLUSH = re.compile(r"\[sim\] flush \((\d+),(\d+)\)-\((\d+),(\d+)\)")


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    app_id: str
    before_ms: int  # first paint done, the app's timer not yet fired
    after_ms: int   # the timer has redrawn its labels


CASES = (
    Case("eink-settings-values", "tdeck-max", "com.thistle.settings",
         500, 3500),
    Case("eink-settings-values-pro", "tdeck-pro", "com.thistle.settings",
         500, 3500),
)


def capture(simulator: Path, case: Case, timeout_ms: int,
            out: Path) -> tuple[bytes, list[int], int]:
    run = subprocess.run(
        [str(simulator), "--headless", "--device", case.device,
         "--timeout", str(timeout_ms), "--launch-app", case.app_id,
         "--screenshot", str(out), "--log-flushes"],
        text=True, capture_output=True, check=False, timeout=60,
    )
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode} at {timeout_ms} ms")
    widths = [int(x2) - int(x1) + 1
              for x1, _, x2, _ in FLUSH.findall(run.stdout + run.stderr)]
    data = out.read_bytes()
    header, pixels = data.split(b"\n255\n", 1)
    width = int(header.splitlines()[-1].split()[0])
    return pixels[width * STATUS_BAR_HEIGHT * 3:], widths, width


def run_case(simulator: Path, workdir: Path, case: Case) -> None:
    before, before_widths, screen_w = capture(
        simulator, case, case.before_ms, workdir / f"{case.name}-before.ppm")
    after, after_widths, _ = capture(
        simulator, case, case.after_ms, workdir / f"{case.name}-after.ppm")
    if not before_widths or set(before_widths) != {screen_w}:
        raise AssertionError(
            f"the first capture is not a full-width paint alone "
            f"(flush widths {sorted(set(before_widths))}); move before_ms")
    if not any(w % 8 for w in after_widths[len(before_widths):]):
        raise AssertionError(
            "no flush narrower than a multiple of 8 pixels before the second "
            "capture, so it cannot show the shear; move after_ms")
    if before != after:
        raise AssertionError(
            "the screen changed after its labels were redrawn on their own: "
            f"{hashlib.sha256(before).hexdigest()[:12]} -> "
            f"{hashlib.sha256(after).hexdigest()[:12]} "
            f"(captures in {workdir})")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    workdir = Path(tempfile.mkdtemp(prefix="epaper-partial-flush-"))
    print("=== E-paper partial flush regressions ===")
    failures = 0
    for case in CASES:
        try:
            run_case(simulator, workdir, case)
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(CASES)} e-paper partial flush tests failed")
        return 1
    print(f"All {len(CASES)} e-paper partial flush tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
