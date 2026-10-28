#!/usr/bin/env python3
"""tools/check_partition_sizes.py measures the kernel against a kernel slot,
and partitions.csv is the table of ADR-0010 (.scratch/ota/spec.md, "The
partition table")."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import check_partition_sizes

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPOSITORY_ROOT / "tools" / "check_partition_sizes.py"
TABLE = REPOSITORY_ROOT / "partitions.csv"

KERNEL_SLOT = 0x480000
RECOVERY_SLOT = 0x280000


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--partition-table", str(TABLE), *args],
                          capture_output=True, text=True, check=False)


class PartitionTableTests(unittest.TestCase):
    def test_the_table_is_two_kernel_slots_and_a_reserved_one(self):
        rows = check_partition_sizes.partitions(TABLE)
        self.assertEqual(rows, [
            ("nvs", "data", "nvs", 0x9000, 0x6000),
            ("otadata", "data", "ota", 0xF000, 0x2000),
            ("phy_init", "data", "phy", 0x11000, 0x1000),
            ("kernel_a", "app", "ota_0", 0x20000, KERNEL_SLOT),
            ("kernel_b", "app", "ota_1", 0x4A0000, KERNEL_SLOT),
            ("recovery", "app", "ota_2", 0x920000, RECOVERY_SLOT),
            ("storage", "data", "spiffs", 0xBA0000, 0x450000),
            ("coredump", "data", "coredump", 0xFF0000, 0x10000),
        ])

    def test_there_is_no_factory_partition(self):
        names = [row[0] for row in check_partition_sizes.partitions(TABLE)]
        self.assertNotIn("factory", names)


class SizeCheckTests(unittest.TestCase):
    def artifact(self, size: int) -> Path:
        handle = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        handle.write(b"\0" * size)
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        return Path(handle.name)

    def test_a_kernel_that_fits_a_kernel_slot_passes(self):
        # Larger than the reserved slot: it is measured against a kernel slot.
        result = run("--firmware", str(self.artifact(RECOVERY_SLOT + 1)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("kernel slot", result.stdout)
        self.assertIn(f"/ {KERNEL_SLOT} bytes", result.stdout)

    def test_a_kernel_larger_than_a_kernel_slot_fails(self):
        result = run("--firmware", str(self.artifact(KERNEL_SLOT + 1)))
        self.assertEqual(result.returncode, 1)
        self.assertIn("exceeds", result.stderr)

    def test_recovery_is_measured_against_its_reserved_slot(self):
        result = run("--recovery", str(self.artifact(RECOVERY_SLOT + 1)))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"limit {RECOVERY_SLOT}", result.stderr)


if __name__ == "__main__":
    unittest.main()
