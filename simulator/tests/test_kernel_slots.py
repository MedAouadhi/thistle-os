#!/usr/bin/env python3
"""The two kernel slots in the simulator (ADR-0010, ticket 03 of
.scratch/ota): a card update is written into the other kernel slot, a
restart boots it, a new kernel is kept once it has run for the keep delay,
and a run that ends before that leaves the next one rolled back.

The simulator's slots are files in THISTLE_SIM_SLOTS (kernel_a.bin,
kernel_b.bin and otadata, the bootloader's choice); the version a slot runs
is the one its image describes. THISTLE_SIM_KEEP_S shortens the keep delay,
and THISTLE_SIM_PUBLISHER_KEY names a key the simulator trusts to sign
firmware. An update is a release as published: the image, its artifact
manifest (tools/sign.py's canonical form) and the manifest's signature.
--install-update installs the card's release once booted, as Settings does,
and the simulator exits where a device restarts.

Every case runs on a scratch copy of simulator/sdcard and its own slots.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from sign import canonical_manifest  # noqa: E402


def kernel_image(version: str, size: int = 8192) -> bytes:
    """Bytes that start as an ESP-IDF app image does: the image header's
    magic, then esp_app_desc_t after the first segment's header, with its
    version."""
    image = bytearray(size)
    image[0] = 0xE9
    image[32:36] = struct.pack("<I", 0xABCD5432)
    image[48:48 + len(version)] = version.encode()
    for i in range(80, size):
        image[i] = i * 7 & 0xFF
    return bytes(image)


class Device:
    """One simulated board: its card, its kernel slots, and the key its
    publisher signs with."""

    def __init__(self, simulator: Path, root: Path):
        self.simulator = simulator
        self.card = root / "card"
        self.slots = root / "slots"
        shutil.copytree(SDCARD, self.card)
        self.key = Ed25519PrivateKey.generate()
        self.public_key = root / "publisher.pub"
        self.public_key.write_bytes(self.key.public_key().public_bytes(
            Encoding.Raw, PublicFormat.Raw))

    def put_update(self, image: bytes, key: Optional[Ed25519PrivateKey] = None,
                   version: str = "0.6.0") -> None:
        update = self.card / "update"
        update.mkdir(exist_ok=True)
        manifest = canonical_manifest(
            artifact_type="firmware", artifact_id="thistle-os", version=version,
            security_version=500, arch="host", compatible_boards=["tdeck-max"],
            url="https://example.com/thistle_os.bin", payload=image)
        (update / "thistle_os.bin").write_bytes(image)
        (update / "thistle_os.bin.manifest").write_bytes(manifest)
        (update / "thistle_os.bin.manifest.sig").write_bytes((key or self.key).sign(manifest))

    def run(self, *args: str, keep_s: float = 60, timeout_ms: int = 1500) -> str:
        env = dict(os.environ,
                   THISTLE_SIM_SDCARD=str(self.card),
                   THISTLE_SIM_SLOTS=str(self.slots),
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

    def install(self) -> str:
        return self.run("--install-update")


def expect(log: str, *lines: str) -> None:
    at = 0
    for line in lines:
        found = log.find(line, at)
        if found < 0:
            raise AssertionError(f"no {line!r}, in order")
        at = found + len(line)


def refuse(log: str, *lines: str) -> None:
    for line in lines:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")


# --- the cases --------------------------------------------------------------

def a_fresh_flash_runs_the_first_kernel_slot(device: Device) -> None:
    log = device.run()
    expect(log, "running kernel_a, version ")
    refuse(log, "new: kept after", "rolled back")


def an_update_is_written_into_the_other_slot_and_boots_after_the_restart(
        device: Device) -> None:
    device.put_update(kernel_image("0.6.0"))
    log = device.install()
    expect(log, "running kernel_a", "into kernel_b",
           "installed 0.6.0 into kernel_b", "restarting into the new kernel")
    if not (device.slots / "kernel_b.bin").is_file():
        raise AssertionError("kernel_b.bin was not written")
    if (device.slots / "kernel_b.bin").read_bytes() != kernel_image("0.6.0"):
        raise AssertionError("kernel_b.bin is not the update")

    log = device.run(keep_s=1, timeout_ms=3000)
    expect(log, "running kernel_b, version 0.6.0, new: kept after 1 s",
           "kept the kernel in kernel_b after 1 s")

    log = device.run()
    expect(log, "running kernel_b, version 0.6.0")
    refuse(log, "new: kept after", "rolled back")


def a_run_ended_before_the_keep_is_rolled_back(device: Device) -> None:
    device.put_update(kernel_image("0.6.0"))
    device.install()

    log = device.run(keep_s=60, timeout_ms=1500)
    expect(log, "running kernel_b, version 0.6.0, new: kept after 60 s")
    refuse(log, "kept the kernel")

    log = device.run()
    expect(log, "the new kernel in kernel_b was not kept; rolled back",
           "running kernel_a, version ")
    # The slot, not the version: the simulator's own is the tag it was
    # built from, which may well start with 0.6.0 too.
    refuse(log, "running kernel_b")

    # And it stays on the first slot.
    log = device.run()
    expect(log, "running kernel_a")
    refuse(log, "rolled back")


def an_update_from_the_second_slot_writes_the_first(device: Device) -> None:
    device.put_update(kernel_image("0.6.0"))
    device.install()
    device.run(keep_s=0, timeout_ms=2500)

    device.put_update(kernel_image("0.7.0"), version="0.7.0")
    log = device.install()
    expect(log, "running kernel_b, version 0.6.0", "into kernel_a",
           "installed 0.7.0 into kernel_a")

    log = device.run()
    expect(log, "running kernel_a, version 0.7.0, new")


def an_update_signed_by_another_key_is_not_written(device: Device) -> None:
    device.put_update(kernel_image("0.6.0"), key=Ed25519PrivateKey.generate())
    log = device.install()
    expect(log, "card update refused: not signed by this device's publisher",
           "Update not installed")
    log = device.run()
    expect(log, "running kernel_a")
    refuse(log, "kernel_b")


def an_update_that_is_no_kernel_is_not_selected(device: Device) -> None:
    device.put_update(b"\x00" * 4096)
    log = device.install()
    expect(log, "is not a kernel ESP-IDF can boot", "Update not installed")
    log = device.run()
    expect(log, "running kernel_a")
    refuse(log, "kernel_b")


CASES = (
    a_fresh_flash_runs_the_first_kernel_slot,
    an_update_is_written_into_the_other_slot_and_boots_after_the_restart,
    a_run_ended_before_the_keep_is_rolled_back,
    an_update_from_the_second_slot_writes_the_first,
    an_update_signed_by_another_key_is_not_written,
    an_update_that_is_no_kernel_is_not_selected,
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
    print("=== Kernel slots ===")
    failures = 0
    for case in cases:
        with tempfile.TemporaryDirectory(prefix="thistle-slots-") as tmp:
            try:
                case(Device(simulator, Path(tmp)))
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.__name__}: {exc}")
            else:
                print(f"PASS  {case.__name__}")

    if failures:
        print(f"{failures}/{len(cases)} kernel slot tests failed")
        return 1
    print(f"All {len(cases)} kernel slot tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
