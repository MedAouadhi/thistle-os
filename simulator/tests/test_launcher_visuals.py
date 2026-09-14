#!/usr/bin/env python3
"""Deterministic framebuffer regression tests for both launcher WMs."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    timeout_ms: int
    tap: str | None
    width: int
    height: int
    expected_sha256: str
    empty_home: bool = False


CASES = (
    Case(
        "eink-home",
        "tdeck-pro",
        2500,
        None,
        240,
        320,
        "fedb795da29d8824bcd6fe1fbab1356ed42ef8924760248160951d53fe13c2b3",
        True,
    ),
    Case(
        "eink-apps",
        "tdeck-pro",
        2500,
        "167,294",
        240,
        320,
        "bb9c21d8d8ddf28b4c911c0d1eb8d38501267995c96eac0a9697b2c62bc1d8a1",
    ),
    Case(
        "lcd-home",
        "tdeck",
        3000,
        None,
        320,
        240,
        "8815a8bc57f7aa32c68db8b2ee0a85ea779a355e981dd8e90a5f425d01b9018e",
        True,
    ),
    # The grid holds the same apps as before lvgl_apps_register_builtin(), in
    # the firmware's registration order rather than the simulator's own:
    # Terminal and Messenger swap places.
    Case(
        "lcd-apps",
        "tdeck",
        3000,
        "159,215",
        320,
        240,
        "1e788863d95cbbc606abef16d8e47a1b182e24f95d00ea6834ec1241bc32ff8e",
    ),
)

STATUS_BAR_HEIGHT = 24
DOCK_HEIGHT = 50


def read_ppm(path: Path) -> tuple[int, int, bytearray]:
    data = path.read_bytes()
    try:
        header, pixels = data.split(b"\n255\n", 1)
        lines = header.splitlines()
        if lines[0] != b"P6":
            raise ValueError("not a binary PPM")
        width, height = (int(value) for value in lines[-1].split())
    except (ValueError, IndexError) as exc:
        raise ValueError(f"invalid PPM: {path}") from exc
    if len(pixels) != width * height * 3:
        raise ValueError(f"incorrect pixel payload length in {path}")
    return width, height, bytearray(pixels)


def masked_hash(width: int, pixels: bytearray) -> str:
    # Clock and battery text are intentionally dynamic. Everything below the
    # system bar is stable and remains under exact framebuffer comparison.
    pixels[: width * STATUS_BAR_HEIGHT * 3] = bytes(width * STATUS_BAR_HEIGHT * 3)
    return hashlib.sha256(pixels).hexdigest()


def assert_empty_home(case: Case, pixels: bytearray) -> None:
    row_bytes = case.width * 3
    start = STATUS_BAR_HEIGHT * row_bytes
    end = (case.height - DOCK_HEIGHT) * row_bytes
    canvas = pixels[start:end]
    colors = Counter(bytes(canvas[i : i + 3]) for i in range(0, len(canvas), 3))
    dominant = colors.most_common(1)[0][1]
    ratio = dominant / (len(canvas) // 3)
    if ratio < 0.995:
        raise AssertionError(
            f"Home canvas is no longer empty: dominant-color ratio {ratio:.4f}"
        )


def run_case(simulator: Path, output_dir: Path, case: Case) -> None:
    screenshot = output_dir / f"{case.name}.ppm"
    command = [
        str(simulator),
        "--headless",
        "--device",
        case.device,
        "--timeout",
        str(case.timeout_ms),
        "--screenshot",
        str(screenshot),
    ]
    if case.tap:
        command.extend(("--tap", case.tap))

    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"simulator exited {completed.returncode}\n{completed.stdout}\n{completed.stderr}"
        )

    width, height, pixels = read_ppm(screenshot)
    if (width, height) != (case.width, case.height):
        raise AssertionError(
            f"expected {case.width}x{case.height}, got {width}x{height}"
        )
    if case.empty_home:
        assert_empty_home(case, pixels)

    actual = masked_hash(width, pixels)
    if actual != case.expected_sha256:
        raise AssertionError(
            f"framebuffer changed\n"
            f"      expected: {case.expected_sha256}\n"
            f"      actual:   {actual}\n"
            f"      capture:  {screenshot}"
        )


# thistle-tk cannot host an LVGL app: those parent their widgets to
# ui_manager_get_app_area(), which is NULL unless an LVGL window manager ran
# ui_manager_init(). lvgl_apps_register_builtin() is therefore called only
# under LVGL, in the firmware and in both simulators. Launching one of these
# on an e-paper board used to segfault; pin the refusal.
#
# These three live in components/apps_builtin and are always compiled, so the
# check cannot go hollow. An app under apps/ would not be: those are absent
# from the simulator builds, and asserting it "does not launch" would pass for
# the wrong reason. run_lvgl_app_case() proves the point by requiring the app
# to launch on the LCD board first.
LVGL_ONLY_APPS = ("com.thistle.settings", "com.thistle.terminal",
                  "com.thistle.assistant")


def launch_app(simulator: Path, device: str, app_id: str):
    return subprocess.run(
        [str(simulator), "--headless", "--timeout", "1500",
         "--device", device, "--launch-app", app_id],
        text=True, capture_output=True, check=False,
    )


def run_lvgl_app_case(simulator: Path, app_id: str) -> None:
    # It must be built, or the e-paper half proves nothing.
    lcd = launch_app(simulator, "tdeck", app_id)
    if lcd.returncode != 0:
        raise AssertionError(
            f"{app_id} does not launch on the LCD board either "
            f"(exit {lcd.returncode}) — it is compiled out, so this case "
            f"would pass for the wrong reason"
        )

    eink = launch_app(simulator, "tdeck-pro", app_id)
    if eink.returncode < 0:
        raise AssertionError(
            f"simulator died on signal {-eink.returncode} launching {app_id} "
            f"under thistle-tk; it should refuse to launch it"
        )
    if eink.returncode == 0:
        raise AssertionError(
            f"{app_id} launched under thistle-tk; it is LVGL-only and the "
            f"device never registers it there"
        )
    # Any non-zero exit would otherwise pass — a bad argument, an unknown
    # device. Pin the refusal itself.
    expected = f"Failed to launch app {app_id}"
    if expected not in eink.stderr:
        raise AssertionError(
            f"expected {expected!r} on stderr, got: "
            f"{eink.stderr.strip()[:200]!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument(
        "--output-dir", type=Path, default=Path("build/launcher-visuals")
    )
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    print("=== Launcher framebuffer regressions ===")
    for case in CASES:
        try:
            run_case(simulator, args.output_dir, case)
        except (AssertionError, RuntimeError, ValueError) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    total = len(CASES) + len(LVGL_ONLY_APPS)
    for app_id in LVGL_ONLY_APPS:
        try:
            run_lvgl_app_case(simulator, app_id)
        except AssertionError as exc:
            failures += 1
            print(f"FAIL  lvgl-unavailable/{app_id}: {exc}")
        else:
            print(f"PASS  lvgl-unavailable/{app_id}")

    if failures:
        print(f"{failures}/{total} launcher visual tests failed")
        return 1
    print(f"All {total} launcher visual tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
