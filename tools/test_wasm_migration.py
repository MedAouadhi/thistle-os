#!/usr/bin/env python3
"""Regression tests for the WASM simulator during the C-to-Rust app migration."""

from pathlib import Path
import unittest


class WasmMigrationTests(unittest.TestCase):
    def setUp(self):
        self.cmake = Path("wasm/CMakeLists.txt").read_text()
        self.main = Path("wasm/main.c").read_text()
        self.workflow = Path(".github/workflows/pages.yml").read_text()

    def test_cmake_filters_sources_removed_after_rust_migration(self):
        self.assertIn("set(THISTLE_WASM_SRCS)", self.cmake)
        self.assertIn('if(EXISTS "${source}")', self.cmake)
        self.assertIn(
            "add_executable(thistle_wasm ${WASM_SRCS} ${THISTLE_WASM_SRCS} ${LVGL_SRCS})",
            self.cmake,
        )

    def test_browser_entrypoint_registers_the_shared_app_list(self):
        # The apps the browser has are the firmware's list, behind the same
        # guards (lvgl_apps.h), not a copy of their own that goes stale.
        self.assertIn('#include "lvgl_apps.h"', self.main)
        self.assertIn("lvgl_apps_register_builtin(", self.main)
        self.assertNotIn("filemgr_app_register();", self.main)

    def test_pull_requests_build_wasm_without_deploying_pages(self):
        self.assertIn("pull_request:", self.workflow)
        self.assertIn("if: github.event_name == 'push'", self.workflow)


if __name__ == "__main__":
    unittest.main()
