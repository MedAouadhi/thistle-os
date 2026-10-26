#!/usr/bin/env python3
"""Settings → System update installs a release copied to the card, and
refuses anything else with its reason (ticket 06 of .scratch/ota, ADR-0010).

A release on the card is what a firmware release publishes: the image, its
artifact manifest (tools/sign.py's canonical form) and the manifest's
signature, in update/. The simulator trusts THISTLE_SIM_PUBLISHER_KEY, a
public key named by an environment variable, in simulator builds only, so
each case signs with a key of its own. Its arch is `host` and its board id
the device's name, tdeck-max.

The simulator's kernel slots, the bootloader's choice and its NVS (the
security floor and each slot's record) are files in THISTLE_SIM_SLOTS;
THISTLE_SIM_KEEP_S shortens the minute a new kernel runs before it is kept.
Each case runs on a scratch copy of simulator/sdcard and slots of its own.

The board profile follows the firmware version (ticket 08): the simulator
reads no profile to set up its board, but keeps one in
THISTLE_SIM_SPIFFS/config/board.json and rewrites it, as a device does, once
it knows the version of the kernel slot it booted. Settings offers Go back
while the other slot still holds the previous version at or above the floor.

    python3 tests/test_system_update.py --sim build/thistle_sim [--only NAME]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from sign import canonical_manifest  # noqa: E402

# tdeck-max, 240x320: Settings' main list, then the System update screen.
SYSTEM_UPDATE = "100,277"
INSTALL = "120,152"
RESTART_NOW = "120,190"
LATER = "120,219"

GO_BACK = "120,166"      # with no release on the card

RELEASE = "thistle_os-host-v0.6.0.bin"
MAX_PROFILE = (Path(__file__).resolve().parents[2]
               / "sdcard_layout/config/boards/tdeck-max.json").read_text()


def kernel_image(version: str, size: int = 8192) -> bytes:
    """Bytes that start as an ESP-IDF app image does, with esp_app_desc_t's
    version where the bootloader reads it."""
    image = bytearray(size)
    image[0] = 0xE9
    image[32:36] = struct.pack("<I", 0xABCD5432)
    image[48:48 + len(version)] = version.encode()
    for i in range(80, size):
        image[i] = i * 7 & 0xFF
    return bytes(image)


class Device:
    """One simulated T-Deck Max: its card, its kernel slots, and the key its
    publisher signs with."""

    def __init__(self, simulator: Path, root: Path):
        self.simulator = simulator
        self.card = root / "card"
        self.slots = root / "slots"
        self.spiffs = root / "spiffs"
        shutil.copytree(SDCARD, self.card)
        self.key = Ed25519PrivateKey.generate()
        self.public_key = root / "publisher.pub"
        self.public_key.write_bytes(self.key.public_key().public_bytes(
            Encoding.Raw, PublicFormat.Raw))

    def put_release(self, version: str = "0.6.0", security_version: int = 600,
                    arch: str = "host", boards: tuple = ("tdeck", "tdeck-max"),
                    key: Optional[Ed25519PrivateKey] = None,
                    on_card: Optional[Callable[[bytes], bytes]] = None) -> None:
        """Copy a release to update/; `on_card` changes the image after it
        is signed for, as a corrupted or swapped copy would."""
        image = kernel_image(version)
        manifest = canonical_manifest(
            artifact_type="firmware", artifact_id="thistle-os", version=version,
            security_version=security_version, arch=arch, compatible_boards=list(boards),
            url=f"https://github.com/MedAouadhi/thistle-os/releases/download/v{version}/{RELEASE}",
            payload=image)
        update = self.card / "update"
        update.mkdir(exist_ok=True)
        (update / RELEASE).write_bytes(on_card(image) if on_card else image)
        (update / f"{RELEASE}.manifest").write_bytes(manifest)
        (update / f"{RELEASE}.manifest.sig").write_bytes((key or self.key).sign(manifest))

    def set_floor(self, floor: int) -> None:
        self.slots.mkdir(exist_ok=True)
        (self.slots / "nvs").write_text(f"fw_sec_ver={floor}\n")

    def nvs(self) -> dict:
        path = self.slots / "nvs"
        if not path.exists():
            return {}
        return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)

    def profile_path(self) -> Path:
        return self.spiffs / "config/board.json"

    def put_profile(self, text: str) -> None:
        self.profile_path().parent.mkdir(parents=True, exist_ok=True)
        self.profile_path().write_text(text)

    def profile(self) -> str:
        return self.profile_path().read_text()

    def backup(self) -> Optional[str]:
        path = self.spiffs / "config/board.json.bak"
        return path.read_text() if path.exists() else None

    def run(self, *args: str, keep_s: float = 60, timeout_ms: int = 1500) -> str:
        env = dict(os.environ,
                   THISTLE_SIM_SDCARD=str(self.card),
                   THISTLE_SIM_SLOTS=str(self.slots),
                   THISTLE_SIM_SPIFFS=str(self.spiffs),
                   THISTLE_SIM_KEEP_S=str(keep_s),
                   THISTLE_SIM_PUBLISHER_KEY=str(self.public_key))
        command = [str(self.simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(timeout_ms), *args]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=timeout_ms / 1000 + 30, env=env)
        log = run.stdout + run.stderr
        if run.returncode != 0:
            raise AssertionError(f"simulator exited {run.returncode}:\n{log[-2000:]}")
        return log

    def settings(self, *taps: str, timeout_ms: int = 9000) -> str:
        """Open Settings → System update and tap through it."""
        args = ["--launch-app", "com.thistle.settings", "--tap", SYSTEM_UPDATE]
        for tap in taps:
            args += ["--tap", tap]
        return self.run(*args, timeout_ms=timeout_ms)


def expect(log: str, *lines: str) -> None:
    at = 0
    for line in lines:
        found = log.find(line, at)
        if found < 0:
            raise AssertionError(f"no {line!r}, in order:\n{log[-3000:]}")
        at = found + len(line)


def refuse(log: str, *lines: str) -> None:
    for line in lines:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")


def nothing_written(device: Device) -> None:
    if (device.slots / "kernel_b.bin").exists():
        raise AssertionError("kernel_b.bin was written")
    log = device.run()
    expect(log, "running kernel_a")
    refuse(log, "kernel_b")


def refused_in_settings(device: Device, reason: str) -> None:
    log = device.settings(INSTALL)
    expect(log, f"card update refused: {reason}",
           f"System update: Not installed: {reason}")
    refuse(log, "restarting into the new kernel")
    nothing_written(device)


def written_by(profile: Optional[str]) -> Optional[str]:
    return None if profile is None else json.loads(profile).get("written_by")


def stamped(profile: str, version: str) -> str:
    """A profile as a firmware of `version` wrote it."""
    return profile.replace("{", '{\n    "written_by": "%s",' % version, 1)


def build_version(log: str) -> str:
    """The version the build's own kernel reports, from `running kernel_a,
    version X`."""
    match = re.search(r"running kernel_a, version (\S+?)(?:,|$)", log, re.M)
    if not match:
        raise AssertionError(f"no running kernel_a in:\n{log[-2000:]}")
    return match.group(1)


def same_profile(text: str, version: str) -> None:
    """`text` is the built-in tdeck-max profile, written by `version`."""
    expected = json.loads(MAX_PROFILE)
    expected["written_by"] = version
    if json.loads(text) != expected:
        raise AssertionError(f"not the built-in profile written by {version}:\n{text[:400]}")


# --- the cases --------------------------------------------------------------

def settings_shows_the_running_version_and_the_release_on_the_card(device: Device) -> None:
    log = device.settings()
    expect(log, "System update: Running: ", "System update: No release on the card")
    device.put_release()
    log = device.settings()
    expect(log, "System update: Running: ", "System update: On the card: 0.6.0")
    refuse(log, "card update")  # nothing installs until asked


def a_release_installs_from_settings_and_restart_now_boots_it(device: Device) -> None:
    device.put_release()
    log = device.settings(INSTALL, RESTART_NOW)
    expect(log, "card update: " + RELEASE,
           "writing 0.6.0 (security version 600, 8192 bytes) into kernel_b",
           "installed 0.6.0 into kernel_b; it runs after the next restart",
           "System update: 0.6.0 is installed. It runs after a restart.",
           "restarting into the new kernel")
    if device.nvs().get("kernel_b_ver") != "0.6.0" or device.nvs().get("kernel_b_sec") != "600":
        raise AssertionError(f"kernel_b's record is not the release's: {device.nvs()}")

    log = device.run(keep_s=1, timeout_ms=3000)
    expect(log, "security floor 0; this kernel's security version 600",
           "running kernel_b, version 0.6.0, new",
           "kept the kernel in kernel_b",
           "security floor raised from 0 to 600")
    if device.nvs().get("fw_sec_ver") != "600":
        raise AssertionError(f"the floor did not rise to 600: {device.nvs()}")


def later_leaves_it_for_the_next_restart(device: Device) -> None:
    device.put_release()
    log = device.settings(INSTALL, LATER)
    expect(log, "System update: 0.6.0 is installed.", "toast: It runs after the next restart")
    refuse(log, "restarting into the new kernel")
    log = device.run()
    expect(log, "running kernel_b, version 0.6.0, new")


def the_floor_rises_only_once_the_new_kernel_is_kept(device: Device) -> None:
    device.put_release(security_version=600)
    device.run("--install-update")
    # A run that ends before the keep: no floor.
    log = device.run(keep_s=60, timeout_ms=1500)
    expect(log, "running kernel_b, version 0.6.0, new")
    refuse(log, "security floor raised")
    if "fw_sec_ver" in device.nvs():
        raise AssertionError(f"the floor rose before the keep: {device.nvs()}")
    # The next run rolls it back, and the floor stays where it was.
    log = device.run()
    expect(log, "rolled back", "running kernel_a")
    refuse(log, "security floor raised")
    if "fw_sec_ver" in device.nvs():
        raise AssertionError(f"a rolled-back kernel raised the floor: {device.nvs()}")


def a_card_release_may_be_older_than_what_runs(device: Device) -> None:
    device.put_release(version="0.1.0", security_version=500)
    log = device.run("--install-update")
    expect(log, "installed 0.1.0 into kernel_b")


def a_bad_signature_is_refused(device: Device) -> None:
    device.put_release(key=Ed25519PrivateKey.generate())
    refused_in_settings(device, "not signed by this device's publisher")


def a_release_for_another_board_is_refused(device: Device) -> None:
    device.put_release(boards=("tdeck", "tdeck-pro"))
    refused_in_settings(device, "it is not for this board, tdeck-max")


def a_release_for_another_chip_is_refused(device: Device) -> None:
    device.put_release(arch="esp32s3")
    refused_in_settings(device, "it is for esp32s3, not this host")


def a_release_below_the_floor_is_refused(device: Device) -> None:
    device.set_floor(700)
    device.put_release(security_version=600)
    refused_in_settings(device, "its security version 600 is below this device's floor 700")


def a_file_of_another_size_is_refused(device: Device) -> None:
    device.put_release(on_card=lambda image: image + b"\0")
    refused_in_settings(device, "the file is 8193 bytes; its manifest says 8192")


def a_file_with_one_byte_changed_is_refused(device: Device) -> None:
    def flip(image: bytes) -> bytes:
        changed = bytearray(image)
        changed[4000] ^= 1
        return bytes(changed)
    device.put_release(on_card=flip)
    refused_in_settings(device, "the file's SHA-256 is not its manifest's")


def a_version_change_rewrites_the_board_profile_and_keeps_the_old_one(device: Device) -> None:
    # Written by an older firmware, and edited since.
    old = stamped(MAX_PROFILE, "0.5.0").replace('"name"', '"name_edited"', 1)
    device.put_profile(old)
    log = device.run()
    version = build_version(log)
    expect(log, f"board profile tdeck-max rewritten from {version}'s built-in copy "
                "(it was 0.5.0's); the previous one is board.json.bak")
    same_profile(device.profile(), version)
    if device.backup() != old:
        raise AssertionError("the previous profile is not the backup")
    # The next boot of the same version leaves it alone.
    log = device.run()
    refuse(log, "board profile")
    same_profile(device.profile(), version)


def a_board_with_no_built_in_profile_keeps_its_own(device: Device) -> None:
    custom = stamped(MAX_PROFILE, "0.5.0").replace('"tdeck-max"', '"my-board"')
    device.put_profile(custom)
    log = device.run()
    refuse(log, "board profile")
    if device.profile() != custom or device.backup() is not None:
        raise AssertionError("a board with no built-in profile had its profile changed")


def go_back_boots_the_other_slot_and_the_profile_follows_it(device: Device) -> None:
    device.put_profile(MAX_PROFILE)
    log = device.run()
    previous = build_version(log)
    expect(log, f"board profile tdeck-max written by {previous}")
    refuse(device.settings(), "Go back")  # nothing to go back to yet

    device.put_release(security_version=500)
    device.run("--install-update")
    (device.card / "update" / RELEASE).unlink()
    log = device.run(keep_s=1, timeout_ms=3000)
    expect(log, "running kernel_b, version 0.6.0",
           f"board profile tdeck-max rewritten from 0.6.0's built-in copy (it was {previous}'s)",
           "kept the kernel in kernel_b")
    same_profile(device.profile(), "0.6.0")

    log = device.settings(GO_BACK)
    expect(log, f"System update: Go back to {previous}",
           f"going back to {previous} in kernel_a; restarting")
    log = device.run(keep_s=1, timeout_ms=3000)
    expect(log, f"running kernel_a, version {previous}, new",
           f"board profile tdeck-max rewritten from {previous}'s built-in copy (it was 0.6.0's)",
           "kept the kernel in kernel_a")
    same_profile(device.profile(), previous)
    if written_by(device.backup()) != "0.6.0":
        raise AssertionError("the backup is not 0.6.0's profile")
    # And from there, forward again: the other slot still holds 0.6.0.
    expect(device.settings(), "System update: Go back to 0.6.0")


def going_back_is_not_offered_below_the_floor(device: Device) -> None:
    device.put_release(security_version=600)
    device.run("--install-update")
    (device.card / "update" / RELEASE).unlink()
    expect(device.run(keep_s=1, timeout_ms=3000), "security floor raised from 0 to 600")
    # The build's own kernel, in kernel_a, is security version 500.
    log = device.settings()
    expect(log, "System update: Running: 0.6.0")
    refuse(log, "Go back")


def go_back_disappears_once_an_install_starts_erasing_the_other_slot(device: Device) -> None:
    device.put_release(security_version=500)
    device.run("--install-update")
    log = device.run(keep_s=1, timeout_ms=3000)
    expect(log, "kept the kernel in kernel_b")
    previous = device.nvs()["kernel_a_ver"]

    # Refused before a byte is written: the previous version is still there.
    device.put_release(version="0.7.0", security_version=500,
                       on_card=lambda image: image + b"\0")
    log = device.settings(INSTALL)
    expect(log, f"System update: Go back to {previous}", "card update refused: the file is")
    refuse(log, "no longer offered")

    # Refused only once it is written, over the previous version.
    def flip(image: bytes) -> bytes:
        changed = bytearray(image)
        changed[4000] ^= 1
        return bytes(changed)
    device.put_release(version="0.7.0", security_version=500, on_card=flip)
    log = device.settings(INSTALL)
    expect(log, f"System update: Go back to {previous}",
           "writing 0.7.0",
           "card update refused: the file's SHA-256 is not its manifest's",
           f"System update: going back to {previous} is no longer offered")
    refuse(device.settings(), "Go back")


CASES = (
    settings_shows_the_running_version_and_the_release_on_the_card,
    a_release_installs_from_settings_and_restart_now_boots_it,
    later_leaves_it_for_the_next_restart,
    the_floor_rises_only_once_the_new_kernel_is_kept,
    a_card_release_may_be_older_than_what_runs,
    a_bad_signature_is_refused,
    a_release_for_another_board_is_refused,
    a_release_for_another_chip_is_refused,
    a_release_below_the_floor_is_refused,
    a_file_of_another_size_is_refused,
    a_file_with_one_byte_changed_is_refused,
    a_version_change_rewrites_the_board_profile_and_keeps_the_old_one,
    a_board_with_no_built_in_profile_keeps_its_own,
    go_back_boots_the_other_slot_and_the_profile_follows_it,
    going_back_is_not_offered_below_the_floor,
    go_back_disappears_once_an_install_starts_erasing_the_other_slot,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.__name__]
    print("=== System update ===")
    failures = 0
    for case in cases:
        with tempfile.TemporaryDirectory(prefix="thistle-update-") as tmp:
            try:
                case(Device(simulator, Path(tmp)))
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.__name__}: {exc}")
            else:
                print(f"PASS  {case.__name__}")

    if failures:
        print(f"{failures}/{len(cases)} system update tests failed")
        return 1
    print(f"All {len(cases)} system update tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
