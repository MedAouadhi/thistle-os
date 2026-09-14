#!/usr/bin/env python3
"""Validate board configs and generated recovery board catalogs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


REQUIRED_BOARD_KEYS = {"name", "arch", "board_id", "version"}
VALID_ARCHES = {"esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6"}

# Bounds mirroring the kernel's board profile parser (board_config.rs). A
# profile the validator accepts but the kernel truncates is a board that comes
# up short of its declaration with nothing reporting it.
MAX_IO_EXPANDERS = 4
MAX_EXPANDER_OUTPUTS = 16
MAX_EXPANDER_PINS = 16
DRIVER_ENTRY_ALIASES = {
    "lcd-st7789-i80.drv.elf": "lcd-st7789.drv.elf",
    "qmi8658.drv.elf": "accel-qmi8658.drv.elf",
    "touch-ft3168.drv.elf": "touch-ft3x68.drv.elf",
}


def driver_sources(repo: Path) -> tuple[set[str], set[str]]:
    known_ids: set[str] = set()
    known_entries: set[str] = set()
    for path in (repo / "sdcard_layout/drivers").glob("*.manifest.json"):
        data = json.loads(path.read_text())
        if data.get("id"):
            known_ids.add(data["id"])
        if data.get("entry"):
            known_entries.add(data["entry"])
    for path in (repo / "components/kernel_rs/src").glob("drv_*.rs"):
        slug = path.stem.removeprefix("drv_").replace("_", "-")
        known_ids.add(f"com.thistle.drv.{slug}")
        known_entries.add(f"{slug}.drv.elf")
    for path in (repo / "components").glob("drv_*"):
        slug = path.name.removeprefix("drv_").replace("_", "-")
        known_ids.add(f"com.thistle.drv.{slug}")
        known_entries.add(f"{slug}.drv.elf")
    return known_ids, known_entries


def parse_address(value: object) -> int | None:
    """Read an I2C address written either bare or as a quoted hex string."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return None
    return None


def validate_io_expanders(label: str, data: dict, known_ids: set[str]) -> list[str]:
    """Validate the IO expander declarations and the pin states applied to them."""
    errors: list[str] = []
    expanders = data.get("io_expanders", [])
    outputs = data.get("expander_outputs", [])

    if not isinstance(expanders, list):
        return [f"{label}: io_expanders must be an array"]
    if not isinstance(outputs, list):
        return [f"{label}: expander_outputs must be an array"]

    if len(expanders) > MAX_IO_EXPANDERS:
        errors.append(f"{label}: at most {MAX_IO_EXPANDERS} io_expanders, got {len(expanders)}")
    if len(outputs) > MAX_EXPANDER_OUTPUTS:
        errors.append(
            f"{label}: at most {MAX_EXPANDER_OUTPUTS} expander_outputs, got {len(outputs)}"
        )
    if outputs and not expanders:
        errors.append(f"{label}: expander_outputs declared with no io_expanders to apply them to")

    for idx, expander in enumerate(expanders):
        where = f"{label}: io_expanders[{idx}]"
        expander_id = expander.get("id")
        if not expander_id:
            errors.append(f"{where} missing id")
        elif expander_id not in known_ids:
            errors.append(f"{where} {expander_id} has no manifest/source driver")
        if not isinstance(expander.get("i2c_bus"), int):
            errors.append(f"{where} missing i2c_bus index")
        if parse_address(expander.get("i2c_addr")) is None:
            errors.append(f"{where} missing or malformed i2c_addr")

    for idx, output in enumerate(outputs):
        where = f"{label}: expander_outputs[{idx}]"
        expander = output.get("expander")
        if not isinstance(expander, int) or isinstance(expander, bool):
            errors.append(f"{where} missing expander index")
        elif not 0 <= expander < len(expanders):
            errors.append(f"{where} expander {expander} is not declared in io_expanders")

        pin = output.get("pin")
        if not isinstance(pin, int) or isinstance(pin, bool):
            errors.append(f"{where} missing pin")
        elif not 0 <= pin < MAX_EXPANDER_PINS:
            errors.append(f"{where} pin {pin} is outside 0-{MAX_EXPANDER_PINS - 1}")

        # Direction is required rather than inferred: the XL9555's
        # configuration registers reset to all-inputs, so a pin given a level
        # but left an input floats and the rail behind it never comes up.
        direction = output.get("direction")
        if direction != "output":
            errors.append(f"{where} direction must be \"output\", got {direction!r}")

        if output.get("level") not in (0, 1):
            errors.append(f"{where} level must be 0 or 1, got {output.get('level')!r}")

        delay_ms = output.get("delay_ms", 0)
        if not isinstance(delay_ms, int) or isinstance(delay_ms, bool) or delay_ms < 0:
            errors.append(f"{where} delay_ms must be a non-negative integer")

    return errors


def validate_board(path: Path, known_ids: set[str], known_entries: set[str]) -> list[str]:
    errors: list[str] = []
    data = json.loads(path.read_text())
    board = data.get("board", {})
    missing = REQUIRED_BOARD_KEYS - set(board)
    if missing:
        errors.append(f"{path}: board missing keys: {', '.join(sorted(missing))}")
    if board.get("arch") not in VALID_ARCHES:
        errors.append(f"{path}: unsupported arch {board.get('arch')!r}")
    if board.get("board_id") != path.stem:
        errors.append(f"{path}: board_id must match filename stem {path.stem!r}")
    if not data.get("drivers"):
        errors.append(f"{path}: must declare at least one driver")

    errors.extend(validate_io_expanders(str(path), data, known_ids))

    for idx, drv in enumerate(data.get("drivers", [])):
        drv_id = drv.get("id")
        if not drv_id:
            errors.append(f"{path}: drivers[{idx}] missing id")
            continue
        entry = drv.get("entry", "")
        aliased_entry = DRIVER_ENTRY_ALIASES.get(entry, entry)
        if drv_id not in known_ids and entry not in known_entries and aliased_entry not in known_entries:
            errors.append(f"{path}: {drv_id} / {entry} has no manifest/source driver")
        if not entry.endswith(".drv.elf"):
            errors.append(f"{path}: {drv_id} entry must end with .drv.elf")
        if "hal" not in drv:
            errors.append(f"{path}: {drv_id} missing hal")
    return errors


def validate_catalog(path: Path, board_ids: set[str]) -> list[str]:
    errors: list[str] = []
    data = json.loads(path.read_text())
    entries = data.get("entries", [])
    catalog_board_ids = {
        entry.get("board_id") or entry.get("id")
        for entry in entries
        if entry.get("type") == "board"
    }
    missing = board_ids - catalog_board_ids
    extra = catalog_board_ids - board_ids
    if missing:
        errors.append(f"{path}: missing board entries: {', '.join(sorted(missing))}")
    if extra:
        errors.append(f"{path}: unknown board entries: {', '.join(sorted(extra))}")
    for entry in entries:
        if entry.get("type") != "board":
            continue
        if not re.match(r"^https?://", entry.get("url", "")):
            errors.append(f"{path}: {entry.get('id')} url must be absolute http(s)")
        if not entry.get("sha256"):
            errors.append(f"{path}: {entry.get('id')} missing sha256")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--catalog", type=Path)
    args = parser.parse_args()

    repo = args.repo.resolve()
    boards = sorted((repo / "sdcard_layout/config/boards").glob("*.json"))
    known_ids, known_entries = driver_sources(repo)
    errors: list[str] = []
    board_ids: set[str] = set()

    for path in boards:
        data = json.loads(path.read_text())
        board_ids.add(data.get("board", {}).get("board_id", path.stem))
        errors.extend(validate_board(path, known_ids, known_entries))

    if args.catalog:
        errors.extend(validate_catalog(args.catalog, board_ids))

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Validated {len(boards)} board configs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
