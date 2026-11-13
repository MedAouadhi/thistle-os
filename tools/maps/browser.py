# SPDX-License-Identifier: BSD-3-Clause
"""The Map Pack tool's browser mode: a page on this computer where the user
draws Regions on an OpenStreetMap map, watches the estimate, saves the
Regions file and starts the build.

    thistle_maps.py serve my-places.geojson

The page is web/index.html, with Leaflet 1.9.4 (BSD-2-Clause) kept in
web/leaflet/, so nothing but the basemap's tiles comes from elsewhere. The
server listens on 127.0.0.1 alone (unless started with --listen) and answers only requests addressed to it
by that name (a page elsewhere cannot reach it through DNS rebinding) that
carry JSON (a form elsewhere cannot post here without the browser asking
first, and the server never says yes).

    GET  /api/state      the Regions file as saved, its path, the Pack's,
                         the budget and the basemap
    POST /api/estimate   a Regions document: the quick estimate
    POST /api/measure    a Regions document: the estimate drawn from the
                         extract, which is read (and fetched) as a build does
    POST /api/save       a Regions document: written to the Regions file
    POST /api/build      builds the Regions file as saved, in the background
    GET  /api/build      how the build is going: its state and its output
"""

from __future__ import annotations

import http.server
import ipaddress
import json
import subprocess
import sys
import threading
from pathlib import Path

import estimate
from regions import RegionsError, parse_regions

HERE = Path(__file__).resolve().parent
WEB = HERE / "web"
TOOL = HERE / "thistle_maps.py"

OSM_TILES = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
OSM_ATTRIBUTION = ('&copy; <a href="https://www.openstreetmap.org/copyright">'
                   'OpenStreetMap</a> contributors')

CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".png": "image/png", ".svg": "image/svg+xml"}

MAX_BODY = 16 << 20  # a Regions document; a hand-drawn one is a few KB


def regions_document(document: dict) -> dict:
    """The document as the Regions file holds it: checked as a build would
    check it, the file's version set, members the page added kept."""
    parse_regions(document)
    out = dict(document)
    out["thistle_regions"] = 1
    return out


class Build:
    """One build at a time, run as `thistle_maps.py build` would be run."""

    def __init__(self, command: list[str]):
        self.command = command
        self.lock = threading.Lock()
        self.state = "idle"  # idle, running, done, failed
        self.log: list[str] = []

    def start(self) -> bool:
        with self.lock:
            if self.state == "running":
                return False
            self.state, self.log = "running", []
        threading.Thread(target=self._run, name="build", daemon=True).start()
        return True

    def _run(self) -> None:
        try:
            proc = subprocess.Popen(self.command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, bufsize=1)
            for line in proc.stdout:
                with self.lock:
                    self.log.append(line.rstrip("\n"))
            code = proc.wait()
        except OSError as exc:
            with self.lock:
                self.log.append(f"cannot start the build: {exc}")
            code = 1
        with self.lock:
            self.state = "done" if code == 0 else "failed"

    def status(self) -> dict:
        with self.lock:
            return {"state": self.state, "log": list(self.log)}


class Session:
    """What the page works on: one Regions file and the Pack built from it."""

    def __init__(self, regions_path: Path, output: Path, budget: int, tiles_url: str,
                 attribution: str, source_args: list[str], measure):
        self.regions_path = regions_path
        self.output = output
        self.budget = budget
        self.tiles_url = tiles_url
        self.attribution = attribution
        self.measure = measure
        self.build = Build([sys.executable, str(TOOL), "build", str(regions_path),
                            "-o", str(output), *source_args])
        self.save_lock = threading.Lock()

    def state(self) -> dict:
        saved = None
        error = None
        if self.regions_path.exists():
            try:
                saved = json.loads(self.regions_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                error = f"cannot read {self.regions_path}: {exc}"
        return {"regions": saved, "error": error, "regions_path": str(self.regions_path),
                "output": str(self.output), "budget": self.budget,
                "tiles_url": self.tiles_url, "attribution": self.attribution}

    def save(self, document: dict) -> dict:
        text = json.dumps(regions_document(document), ensure_ascii=False, indent=2) + "\n"
        with self.save_lock:
            tmp = self.regions_path.with_name(self.regions_path.name + ".tmp")
            tmp.write_text(text, encoding="utf-8")
            tmp.replace(self.regions_path)
        return {"saved": str(self.regions_path)}


def make_handler(session: Session, lan: bool = False, allow_hosts=()):
    """lan: the server listens beyond loopback, so a Host that is an IP
    address is answered too; a name still is not unless allow_hosts has it
    (a name is what DNS rebinding needs)."""
    names = {"127.0.0.1", "localhost", *allow_hosts}

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "thistle-maps"

        def log_message(self, *args):
            pass

        def _send(self, code: int, body: bytes, content_type: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code: int, value) -> None:
            self._send(code, json.dumps(value).encode("utf-8"), "application/json")

        def _ours(self) -> bool:
            host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
            if host in names:
                return True
            if lan:
                try:
                    ipaddress.ip_address(host.strip("[]"))
                    return True
                except ValueError:
                    pass
            self._json(403, {"error": "this server answers only on the addresses it was started for"})
            return False

        def do_GET(self):
            if not self._ours():
                return
            path = self.path.split("?", 1)[0]
            if path == "/api/state":
                return self._json(200, session.state())
            if path == "/api/build":
                return self._json(200, session.build.status())
            if path == "/":
                path = "/index.html"
            target = (WEB / path.lstrip("/")).resolve()
            if WEB.resolve() not in target.parents or not target.is_file():
                return self._json(404, {"error": "not found"})
            self._send(200, target.read_bytes(),
                       CONTENT_TYPES.get(target.suffix, "application/octet-stream"))

        def do_POST(self):
            if not self._ours():
                return
            if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                return self._json(415, {"error": "send JSON"})
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_BODY:
                return self._json(413, {"error": "too big"})
            try:
                body = json.loads(self.rfile.read(length) or b"null")
            except json.JSONDecodeError as exc:
                return self._json(400, {"error": f"not JSON: {exc}"})
            path = self.path.split("?", 1)[0]
            try:
                if path == "/api/estimate":
                    result = estimate.estimate_pack(parse_regions(body))
                    return self._json(200, result.as_json(session.budget))
                if path == "/api/measure":
                    result = session.measure(parse_regions(body))
                    return self._json(200, result.as_json(session.budget))
                if path == "/api/save":
                    return self._json(200, session.save(body))
                if path == "/api/build":
                    if not session.build.start():
                        return self._json(409, {"error": "a build is running"})
                    return self._json(202, {"output": str(session.output)})
            except RegionsError as exc:
                return self._json(400, {"error": str(exc)})
            except (LookupError, OSError, RuntimeError) as exc:
                return self._json(500, {"error": str(exc)})
            return self._json(404, {"error": "not found"})

    return Handler


def serve(session: Session, port: int, open_browser: bool,
          listen: str = "127.0.0.1", allow_hosts=()) -> int:
    lan = listen not in ("127.0.0.1", "localhost", "::1")
    server = http.server.ThreadingHTTPServer((listen, port),
                                             make_handler(session, lan, allow_hosts))
    server.daemon_threads = True
    url = f"http://{'127.0.0.1' if listen in ('0.0.0.0', '') else listen}:{server.server_address[1]}/"
    print(f"The Map Pack tool is at {url}", flush=True)
    if lan:
        print(f"Listening on {listen}: anyone who can reach this port can change the "
              "Regions file and start builds; there is no sign-in. Use it on a "
              "network you trust.", flush=True)
    print(f"Regions file {session.regions_path}, Pack {session.output}; Ctrl-C stops it",
          flush=True)
    if open_browser:
        import webbrowser

        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
