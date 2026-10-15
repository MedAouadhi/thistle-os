#!/usr/bin/env python3
"""Every built-in app launches on every simulated device profile.

The launcher's framebuffer tests show that the right icons are drawn; they do
not open anything. This opens each app, under the window manager the device
boots by default, and requires it to come up and survive a couple of seconds
of timers.

It exists because e-paper boards used to default to thistle-tk, which hosts
three of the fourteen apps of the time, and nothing noticed that the T-Deck Pro and Max
had lost the other eleven.

Each LVGL app's first screen must also fit the display: nothing may reach past
its left or right edge, where on a panel it is simply not there. The App
Store's tabs and buttons, laid out for 320 px, ran off a 240 px T-Deck Max.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Device:
    name: str
    has_radio: bool
    has_gps: bool
    # Whether every app's first screen must fit the display's width.
    fits_screen: bool = True


# One device per distinct profile in simulator/board_simulator.c: tdeck-plus
# is the tdeck again, and t3-s3 and rak3312 are the heltec-v3. The simulators
# share one sdcard symlink per run, so these run serially and duplicates only
# cost time. Keep the capabilities in step with that table.
DEVICES = (
    Device("tdeck", True, True),        # LCD 320x240
    Device("tdeck-pro", True, True),    # e-paper 240x320
    Device("tdeck-max", True, True),    # e-paper, CST66xx touch, XL9555
    Device("tdisplay", False, False),   # LCD 320x170, touch only
    # OLED 128x64. Not held to the width check: the status bar alone is
    # wider than this screen, and so is nearly every app laid out for 240 px.
    Device("heltec-v3", True, False, fits_screen=False),
    Device("cardputer", False, False),  # LCD 240x135, keyboard only
)

UNGATED_APPS = (
    "launcher", "settings", "filemgr", "reader", "podcasts", "notes",
    "appstore", "assistant", "flashlight", "weather", "terminal", "vault",
    "phone", "filexfer",
)
# lvgl_apps_register_builtin() registers these only on a capable board.
RADIO_APPS = ("messenger", "wifiscanner")
GPS_APPS = ("navigator",)
# All thistle-tk hosts. Its App Store and Flashlight register only under it,
# so these also catch that registration going missing.
TK_APPS = ("tk_launcher", "appstore", "flashlight")


def launch(simulator: Path, device: str, app_id: str, wm: str | None = None):
    extra = ["--wm", wm] if wm else []
    return subprocess.run(
        [str(simulator), "--headless", "--timeout", "2000",
         "--device", device, "--launch-app", app_id, "--report-offscreen",
         *extra],
        text=True, capture_output=True, check=False, timeout=60,
    )


def check_launches(simulator: Path, device: Device, app_id: str,
                   wm: str | None = None) -> None:
    run = launch(simulator, device.name, app_id, wm)
    if run.returncode < 0:
        raise AssertionError(f"simulator died on signal {-run.returncode}")
    if run.returncode != 0:
        raise AssertionError(
            f"exit {run.returncode}: {run.stderr.strip()[-300:]!r}")
    if f"App launched: {app_id}" not in run.stdout:
        raise AssertionError("exited 0 without reporting the launch")
    # The report walks LVGL's object tree, so thistle-tk has none to give.
    if device.fits_screen and wm is None:
        check_fits(run.stdout)


def check_fits(stdout: str) -> None:
    """Fail on any object the simulator reports past the screen's edge."""
    if "Off-screen objects: " not in stdout:
        raise AssertionError("simulator did not report off-screen objects")
    offscreen = [line for line in stdout.splitlines()
                 if line.startswith("Off screen: ")]
    if offscreen:
        raise AssertionError("wider than the screen: " + "; ".join(offscreen))


def check_refused(simulator: Path, device: str, app_id: str) -> None:
    run = launch(simulator, device, app_id)
    if run.returncode < 0:
        raise AssertionError(f"simulator died on signal {-run.returncode}")
    if f"Failed to launch app {app_id}" not in run.stderr:
        raise AssertionError(
            f"expected the board to lack it, got exit {run.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Built-in app launch matrix ===")
    total = failures = 0
    for device in DEVICES:
        cases = [(app, True, None) for app in UNGATED_APPS]
        cases += [(app, device.has_radio, None) for app in RADIO_APPS]
        cases += [(app, device.has_gps, None) for app in GPS_APPS]
        cases += [(app, True, "thistle-tk") for app in TK_APPS]
        for app, expected, wm in cases:
            app_id = f"com.thistle.{app}"
            label = f"{device.name}/{app}"
            if wm:
                label += f" [{wm}]"
            if not expected:
                label += " (absent)"
            total += 1
            try:
                if expected:
                    check_launches(simulator, device, app_id, wm)
                else:
                    check_refused(simulator, device.name, app_id)
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {label}: {exc}")
            else:
                print(f"PASS  {label}")

    if failures:
        print(f"{failures}/{total} app launch cases failed")
        return 1
    print(f"All {total} app launch cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
