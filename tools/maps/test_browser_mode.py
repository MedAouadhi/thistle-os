#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The Map Pack tool's browser mode, driven through a real browser: the
user's page, mouse and keys, the Regions file it saves and the Pack it
builds. The Pack is read with test_thistle_maps.py's own reader.

    pip install -r tools/maps/requirements.txt playwright
    python3 -m playwright install --with-deps chromium
    python3 tools/maps/test_browser_mode.py

The basemap is a blank tile served here, so the test needs no network.
"""

from __future__ import annotations

import http.server
import json
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from test_thistle_maps import EXTRACT, REGIONS, Pack, metres  # noqa: E402

try:
    from playwright.sync_api import expect, sync_playwright
except ImportError:  # pragma: no cover - reported by main()
    sync_playwright = None

TOOL = HERE / "thistle_maps.py"


def blank_png() -> bytes:
    """A 256 x 256 light grey PNG, as a basemap tile."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = b"".join(b"\x00" + b"\xee" * 256 for _ in range(256))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class _Tiles(http.server.BaseHTTPRequestHandler):
    png = blank_png()

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(self.png)))
        self.end_headers()
        self.wfile.write(self.png)

    def log_message(self, *args):
        pass


def regions_in(path: Path) -> list[dict]:
    """The Regions of a saved file, as the format document has them."""
    document = json.loads(path.read_text(encoding="utf-8"))
    assert document["type"] == "FeatureCollection"
    assert document["thistle_regions"] == 1
    out = []
    for feature in document["features"]:
        props, geometry = feature["properties"], feature["geometry"]
        region = {"name": props.get("name"), "deepest_zoom": props.get("deepest_zoom", 16),
                  "kind": geometry["type"]}
        if geometry["type"] == "Point":
            region["centre"] = (geometry["coordinates"][1], geometry["coordinates"][0])
            region["radius_m"] = props["radius_m"]
        else:
            region["ring"] = [(lat, lon) for lon, lat in geometry["coordinates"][0]]
        out.append(region)
    return out


class BrowserMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.tiles = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Tiles)
        threading.Thread(target=cls.tiles.serve_forever, daemon=True).start()
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()
        cls.tiles.shutdown()
        cls.tiles.server_close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=self.tmp))
        self.regions = self.dir / "places.geojson"
        self.pack = self.dir / "places.tmap"

    def tearDown(self):
        if getattr(self, "page", None):
            self.page.close()
        if getattr(self, "server", None):
            self.server.terminate()
            self.server.wait(timeout=10)

    # -- helpers ------------------------------------------------------------

    def open_tool(self, *extra: str):
        port = self.tiles.server_address[1]
        self.server = subprocess.Popen(
            [sys.executable, str(TOOL), "serve", str(self.regions), "--port", "0", "--no-browser",
             "--extract", str(EXTRACT), "--tiles", f"http://127.0.0.1:{port}/{{z}}/{{x}}/{{y}}.png",
             *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.server.stdout.readline()
        match = re.search(r"(http://127\.0\.0\.1:\d+/)", line)
        self.assertIsNotNone(match, f"the tool said: {line!r}")
        self.url = match.group(1)
        self.page = self.browser.new_page(viewport={"width": 1200, "height": 800})
        self.page.goto(self.url)
        expect(self.page.locator("#estimate")).to_contain_text("tiles")

    def regions_listed(self) -> list[str]:
        return [n.input_value() for n in self.page.locator("#regions .region .name").all()]

    def map_box(self):
        return self.page.locator("#map").bounding_box()

    def latlng_at(self, x: float, y: float) -> tuple[float, float]:
        """The map's latitude and longitude under a point of the page."""
        box = self.map_box()
        ll = self.page.evaluate("([x, y]) => { const ll = window.thistleMaps.map"
                                ".containerPointToLatLng([x, y]); return [ll.lat, ll.lng]; }",
                                [x - box["x"], y - box["y"]])
        return ll[0], ll[1]

    def drag(self, points: list[tuple[float, float]]) -> None:
        mouse = self.page.mouse
        mouse.move(*points[0])
        mouse.down()
        for p in points[1:]:
            mouse.move(*p, steps=4)
        mouse.up()

    def save(self) -> None:
        self.page.click("#save")
        expect(self.page.locator("#status")).to_contain_text("Saved")

    def estimate_cli(self, path: Path, *extra: str) -> dict:
        run = subprocess.run([sys.executable, str(TOOL), "estimate", str(path), "--json", *extra],
                             capture_output=True, text=True, check=True)
        return json.loads(run.stdout)

    # -- cases ----------------------------------------------------------------

    def test_a_saved_file_opens_and_saves_unchanged(self):
        shutil.copy(REGIONS, self.regions)
        self.open_tool()
        self.assertEqual(self.regions_listed(), ["Medina", "Lac Nord"])
        self.save()
        before, after = regions_in(REGIONS), regions_in(self.regions)
        self.assertEqual(len(after), 2)
        for a, b in zip(before, after):
            self.assertEqual((a["name"], a["deepest_zoom"], a["kind"]),
                             (b["name"], b["deepest_zoom"], b["kind"]))
            if a["kind"] == "Point":
                self.assertEqual(a["radius_m"], b["radius_m"])
                self.assertLess(metres(a["centre"], b["centre"]), 0.05)
            else:
                self.assertEqual(len(a["ring"]), len(b["ring"]))
                for p, q in zip(a["ring"], b["ring"]):
                    self.assertLess(metres(p, q), 0.05)

    def test_draw_edit_and_delete_regions_then_save(self):
        shutil.copy(REGIONS, self.regions)
        self.open_tool()
        box = self.map_box()
        cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2

        # A circle: press at its centre, let go on its edge.
        self.page.click("#tool-circle")
        centre, edge = (cx - 200, cy - 150), (cx - 140, cy - 150)
        expect_centre, expect_edge = self.latlng_at(*centre), self.latlng_at(*edge)
        self.drag([centre, (cx - 170, cy - 150), edge])
        self.assertEqual(self.regions_listed(), ["Medina", "Lac Nord", "Region 3"])

        # A freehand polygon: a loop drawn with the button held.
        self.page.click("#tool-freehand")
        loop = [(cx + 100 + 80 * math.cos(a / 12 * 2 * math.pi),
                 cy + 120 + 60 * math.sin(a / 12 * 2 * math.pi)) for a in range(13)]
        self.drag(loop)
        self.assertEqual(self.regions_listed(), ["Medina", "Lac Nord", "Region 3", "Region 4"])

        # Edit: name the circle and set its depth; set the polygon's depth.
        third = self.page.locator("#regions .region").nth(2)
        third.locator(".name").fill("Souk")
        third.locator(".depth").select_option("14")
        self.page.locator("#regions .region").nth(3).locator(".depth").select_option("17")

        # Delete Lac Nord.
        self.page.locator("#regions .region").nth(1).locator(".delete").click()
        self.assertEqual(self.regions_listed(), ["Medina", "Souk", "Region 4"])
        self.save()

        saved = regions_in(self.regions)
        self.assertEqual([(r["name"], r["deepest_zoom"], r["kind"]) for r in saved],
                         [("Medina", 17, "Point"), ("Souk", 14, "Point"), ("Region 4", 17, "Polygon")])
        souk = saved[1]
        self.assertLess(metres(souk["centre"], expect_centre), 3)
        self.assertAlmostEqual(souk["radius_m"], metres(expect_centre, expect_edge),
                               delta=0.05 * metres(expect_centre, expect_edge))
        ring = saved[2]["ring"]
        self.assertGreaterEqual(len(ring), 5)
        self.assertEqual(ring[0], ring[-1])  # GeoJSON closes its rings
        lats = [p[0] for p in ring]
        top, bottom = self.latlng_at(cx + 100, cy + 60), self.latlng_at(cx + 100, cy + 180)
        self.assertAlmostEqual(max(lats), top[0], delta=abs(top[0] - bottom[0]) * 0.1)
        self.assertAlmostEqual(min(lats), bottom[0], delta=abs(top[0] - bottom[0]) * 0.1)

        # Reopened later, the file shows what was saved.
        self.page.reload()
        expect(self.page.locator("#estimate")).to_contain_text("tiles")
        self.assertEqual(self.regions_listed(), ["Medina", "Souk", "Region 4"])

    def test_the_estimate_follows_the_drawing_and_warns_above_the_budget(self):
        shutil.copy(REGIONS, self.regions)
        self.open_tool("--budget", "100KB")
        quick = self.estimate_cli(REGIONS)
        expect(self.page.locator("#estimate")).to_contain_text(f"{quick['tiles']} tiles")
        expect(self.page.locator("#warning")).to_be_hidden()

        # Medina to z14 alone: fewer tiles, still under the budget.
        self.page.locator("#regions .region").nth(0).locator(".depth").select_option("14")
        shallow = json.loads(REGIONS.read_text())
        shallow["features"][0]["properties"]["deepest_zoom"] = 14
        (self.dir / "shallow.geojson").write_text(json.dumps(shallow))
        fewer = self.estimate_cli(self.dir / "shallow.geojson")
        expect(self.page.locator("#estimate")).to_contain_text(f"{fewer['tiles']} tiles")
        expect(self.page.locator("#warning")).to_be_hidden()

        # A wide circle to z17 passes 100 KB.
        self.page.click("#tool-circle")
        box = self.map_box()
        cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
        self.drag([(cx, cy), (cx + 150, cy)])
        self.page.locator("#regions .region").nth(2).locator(".depth").select_option("17")
        expect(self.page.locator("#warning")).to_be_visible()
        expect(self.page.locator("#warning")).to_contain_text("100 KB")

    def test_build_from_the_page_and_measure_the_estimate(self):
        shutil.copy(REGIONS, self.regions)
        self.open_tool()
        self.page.click("#measure")
        expect(self.page.locator("#estimate")).to_contain_text("measured", timeout=60_000)
        measured = int(self.page.locator("#estimate").get_attribute("data-bytes"))

        self.page.click("#build")
        expect(self.page.locator("#status")).to_contain_text("Built", timeout=120_000)
        expect(self.page.locator("#log")).to_contain_text("wrote")
        built = self.pack.read_bytes()
        pack = Pack(built)
        self.assertEqual([r["name"] for r in pack.regions()], ["Medina", "Lac Nord"])
        self.assertLessEqual(abs(measured - len(built)), 0.05 * len(built))

    def test_a_drawing_is_built_as_saved(self):
        """Build saves first: what is built is what is on the screen."""
        self.open_tool()
        self.assertEqual(self.regions_listed(), [])
        # Show the fixture's area, then draw a circle over the Medina.
        self.page.evaluate("window.thistleMaps.map.setView([36.80, 10.18], 16)")
        box = self.map_box()
        cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
        self.page.click("#tool-circle")
        self.drag([(cx, cy), (cx + 60, cy)])
        self.page.click("#build")
        expect(self.page.locator("#status")).to_contain_text("Built", timeout=120_000)
        pack = Pack(self.pack.read_bytes())
        (region,) = pack.regions()
        self.assertEqual((region["name"], region["kind"], region["deepest_zoom"]),
                         ("Region 1", "circle", 16))
        self.assertLess(metres(region["centre"], (36.80, 10.18)), 3)

    def test_only_this_computer_s_pages_can_save(self):
        """A page from elsewhere can neither post a form here nor reach
        the server through a name of its own (DNS rebinding)."""
        import urllib.error
        import urllib.request

        shutil.copy(REGIONS, self.regions)
        before = self.regions.read_bytes()
        self.open_tool()
        document = json.dumps({"type": "FeatureCollection", "features": []}).encode()
        for headers, code in (({"Content-Type": "text/plain"}, 415),
                              ({"Content-Type": "application/json", "Host": "evil.example"}, 403)):
            request = urllib.request.Request(self.url + "api/save", data=document,
                                             headers=headers, method="POST")
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(request, timeout=10)
            self.assertEqual(caught.exception.code, code)
        self.assertEqual(self.regions.read_bytes(), before)

    def test_a_region_the_build_cannot_use_is_reported_not_saved(self):
        self.open_tool()
        self.page.click("#save")
        expect(self.page.locator("#status")).to_contain_text("no Regions")
        self.assertFalse(self.regions.exists())


def main() -> int:
    if sync_playwright is None:
        print("SKIP: the browser mode's test needs playwright "
              "(pip install playwright; python3 -m playwright install --with-deps chromium)")
        return 0
    result = unittest.main(verbosity=2, exit=False).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
