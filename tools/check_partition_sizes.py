#!/usr/bin/env python3
"""Check firmware artifacts against the partition table's app slots: the
kernel against a kernel slot (`kernel_a`; `kernel_b` is the same size), and
Recovery against its reserved slot (ADR-0010)."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

KERNEL_SLOTS = ("kernel_a", "kernel_b")
RECOVERY_SLOT = "recovery"


def parse_size(value: str) -> int:
    value = value.strip()
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def partitions(path: Path) -> list[tuple[str, str, str, int, int]]:
    """Each row's name, type, subtype, offset and size."""
    rows = []
    for row in csv.reader(
        line for line in path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        if len(row) < 5:
            continue
        name, kind, subtype = (field.strip() for field in row[:3])
        rows.append((name, kind, subtype, parse_size(row[3]), parse_size(row[4])))
    return rows


def partition_sizes(path: Path) -> dict[str, int]:
    return {row[0]: row[4] for row in partitions(path)}


def check(label: str, artifact: Path, limit: int) -> list[str]:
    if not artifact.exists():
        return [f"{label}: missing artifact {artifact}"]
    size = artifact.stat().st_size
    free = limit - size
    pct = size * 100 // limit
    print(f"{label}: {size} / {limit} bytes ({pct}% used, {free} free)")
    if size > limit:
        return [f"{label}: {size} exceeds partition limit {limit}"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition-table", type=Path, default=Path("partitions.csv"))
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--recovery", type=Path)
    args = parser.parse_args()

    sizes = partition_sizes(args.partition_table)
    errors: list[str] = []
    if args.firmware:
        missing = [slot for slot in KERNEL_SLOTS if slot not in sizes]
        if missing:
            errors.append(f"partition table has no {', '.join(missing)}")
        else:
            # The kernel has to fit whichever slot it is written into.
            limit = min(sizes[slot] for slot in KERNEL_SLOTS)
            errors.extend(check("firmware, kernel slot", args.firmware, limit))
    if args.recovery:
        if RECOVERY_SLOT not in sizes:
            errors.append(f"partition table has no {RECOVERY_SLOT}")
        else:
            errors.extend(check("recovery, reserved slot", args.recovery,
                                sizes[RECOVERY_SLOT]))

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
