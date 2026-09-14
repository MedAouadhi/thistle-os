#!/usr/bin/env python3
"""Tests for board profile validation."""

import unittest

from validate_board_catalog import validate_io_expanders


KNOWN_IDS = {"com.thistle.drv.ioexp-xl9555"}

EXPANDER = {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20"}


def profile(**overrides) -> dict:
    data = {
        "io_expanders": [dict(EXPANDER)],
        "expander_outputs": [
            {"expander": 0, "pin": 1, "direction": "output", "level": 1, "delay_ms": 0}
        ],
    }
    data.update(overrides)
    return data


def errors_for(**overrides) -> list[str]:
    return validate_io_expanders("board.json", profile(**overrides), KNOWN_IDS)


class IoExpanderValidationTests(unittest.TestCase):
    def test_a_well_formed_declaration_passes(self):
        self.assertEqual(errors_for(), [])

    def test_a_profile_declaring_no_expanders_passes(self):
        self.assertEqual(validate_io_expanders("board.json", {}, KNOWN_IDS), [])

    def test_a_decimal_address_is_accepted_alongside_quoted_hex(self):
        self.assertEqual(errors_for(io_expanders=[{**EXPANDER, "i2c_addr": 32}]), [])

    def test_an_expander_needs_an_address(self):
        errors = errors_for(io_expanders=[{k: v for k, v in EXPANDER.items() if k != "i2c_addr"}])
        self.assertTrue(any("i2c_addr" in error for error in errors))

    def test_a_malformed_address_is_rejected(self):
        errors = errors_for(io_expanders=[{**EXPANDER, "i2c_addr": "0xZZ"}])
        self.assertTrue(any("i2c_addr" in error for error in errors))

    def test_an_expander_needs_a_bus_index(self):
        errors = errors_for(io_expanders=[{k: v for k, v in EXPANDER.items() if k != "i2c_bus"}])
        self.assertTrue(any("i2c_bus" in error for error in errors))

    def test_an_expander_needs_a_driver_that_exists(self):
        errors = errors_for(io_expanders=[{**EXPANDER, "id": "com.thistle.drv.ioexp-imaginary"}])
        self.assertTrue(any("no manifest/source driver" in error for error in errors))

    def test_a_pin_entry_without_a_direction_is_rejected(self):
        errors = errors_for(expander_outputs=[{"expander": 0, "pin": 1, "level": 1}])
        self.assertTrue(any("direction must be" in error for error in errors))

    def test_an_unrecognised_direction_is_rejected(self):
        errors = errors_for(
            expander_outputs=[{"expander": 0, "pin": 1, "direction": "input", "level": 1}]
        )
        self.assertTrue(any("direction must be" in error for error in errors))

    def test_a_pin_entry_without_a_level_is_rejected(self):
        errors = errors_for(expander_outputs=[{"expander": 0, "pin": 1, "direction": "output"}])
        self.assertTrue(any("level must be 0 or 1" in error for error in errors))

    def test_an_output_pin_needs_a_level_of_zero_or_one(self):
        errors = errors_for(
            expander_outputs=[{"expander": 0, "pin": 1, "direction": "output", "level": 2}]
        )
        self.assertTrue(any("level must be 0 or 1" in error for error in errors))

    def test_a_pin_beyond_the_expander_width_is_rejected(self):
        errors = errors_for(
            expander_outputs=[{"expander": 0, "pin": 16, "direction": "output", "level": 1}]
        )
        self.assertTrue(any("outside 0-15" in error for error in errors))

    def test_a_pin_entry_without_a_pin_is_rejected(self):
        errors = errors_for(expander_outputs=[{"expander": 0, "direction": "output", "level": 1}])
        self.assertTrue(any("missing pin" in error for error in errors))

    def test_an_undeclared_expander_index_is_rejected(self):
        errors = errors_for(
            expander_outputs=[{"expander": 1, "pin": 1, "direction": "output", "level": 1}]
        )
        self.assertTrue(any("not declared in io_expanders" in error for error in errors))

    def test_pin_states_without_an_expander_to_apply_them_to_are_rejected(self):
        errors = errors_for(io_expanders=[])
        self.assertTrue(any("no io_expanders" in error for error in errors))

    def test_a_negative_delay_is_rejected(self):
        errors = errors_for(
            expander_outputs=[
                {"expander": 0, "pin": 1, "direction": "output", "level": 1, "delay_ms": -1}
            ]
        )
        self.assertTrue(any("delay_ms" in error for error in errors))

    def test_more_expanders_than_the_kernel_parses_are_rejected(self):
        errors = errors_for(io_expanders=[dict(EXPANDER) for _ in range(5)])
        self.assertTrue(any("at most 4 io_expanders" in error for error in errors))

    def test_more_pin_states_than_the_kernel_parses_are_rejected(self):
        entry = {"expander": 0, "pin": 1, "direction": "output", "level": 1}
        errors = errors_for(expander_outputs=[dict(entry) for _ in range(17)])
        self.assertTrue(any("at most 16 expander_outputs" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
