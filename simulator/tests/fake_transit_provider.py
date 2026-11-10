#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""A fake vbb-rest 6 (https://v6.vbb.transport.rest), the Berlin
Provider's server, for test_transit.py: stdlib only, on 127.0.0.1.

It answers the three calls the Provider makes, in vbb-rest's own shapes
(ticket 07's research has them):

    GET /locations/nearby?latitude=&longitude=&distance=&results=  stops
    GET /radar?north=&west=&south=&east=&results=&frames=0          movements
    GET /stops/:id/departures?duration=&results=                    departures

with stops, vehicles and departures a test sets. `vehicles` may be a list
of lists, the n-th /radar request answering the n-th (the last for the
rest), so a test moves a vehicle between fetches. `fail_radar` lists the
/radar requests (1 for the first) that answer 500. Every request is kept,
with its time, in `requests`.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import parse_qs, urlsplit


@dataclass
class Stop:
    id: str
    name: str
    lat: float
    lon: float


@dataclass
class Vehicle:
    line: str
    direction: str
    lat: float
    lon: float


@dataclass
class Departure:
    when: Optional[str]        # ISO 8601 with offset, None without realtime
    planned: str
    delay: Optional[int]       # seconds, None without realtime
    line: str
    direction: str


@dataclass
class Request:
    path: str
    query: dict[str, str]
    at: float


@dataclass
class FakeTransit:
    stops: list[Stop] = field(default_factory=list)
    vehicles: list[list[Vehicle]] = field(default_factory=list)
    departures: dict[str, list[Departure]] = field(default_factory=dict)
    fail_radar: tuple[int, ...] = ()
    requests: list[Request] = field(default_factory=list)

    def start(self) -> "FakeTransit":
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                pass

            def do_GET(self) -> None:
                parts = urlsplit(self.path)
                query = {k: v[0] for k, v in parse_qs(parts.query).items()}
                fake.requests.append(Request(parts.path, query, time.monotonic()))
                status, body = fake.answer(parts.path, query)
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "public, max-age=30")
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.url = f"http://127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def asked(self, path_start: str) -> list[Request]:
        return [r for r in self.requests if r.path.startswith(path_start)]

    def answer(self, path: str, q: dict[str, str]) -> tuple[int, object]:
        if path == "/locations/nearby":
            return 200, [{
                "type": "stop", "id": s.id, "name": s.name,
                "location": {"type": "location", "id": s.id,
                             "latitude": s.lat, "longitude": s.lon},
                "products": {"suburban": False, "subway": True, "tram": True, "bus": True,
                             "ferry": False, "express": False, "regional": False},
                "distance": 100,
            } for s in self.stops]
        if path == "/radar":
            n = len(self.asked("/radar"))
            if n in self.fail_radar:
                return 500, {"message": "upstream failed"}
            if not self.vehicles:
                moving: list[Vehicle] = []
            else:
                moving = self.vehicles[min(n, len(self.vehicles)) - 1]
            return 200, {"movements": [{
                "direction": v.direction, "tripId": f"1|{i}|0|86|10112026",
                "line": {"type": "line", "id": v.line.lower(), "name": v.line,
                         "product": "tram", "mode": "train"},
                "location": {"type": "location", "latitude": v.lat, "longitude": v.lon},
                "nextStopovers": [], "frames": [],
            } for i, v in enumerate(moving)], "realtimeDataUpdatedAt": int(time.time())}
        if path.startswith("/stops/") and path.endswith("/departures"):
            stop = path[len("/stops/"):-len("/departures")]
            if stop not in self.departures:
                return 404, {"message": "stop not found"}
            return 200, {"departures": [{
                "tripId": f"1|{i}|0|86|10112026",
                "stop": {"type": "stop", "id": stop},
                "when": d.when, "plannedWhen": d.planned, "delay": d.delay,
                "platform": None, "plannedPlatform": None, "prognosisType": "prognosed",
                "direction": d.direction,
                "line": {"type": "line", "name": d.line, "product": "tram"},
                "remarks": [],
            } for i, d in enumerate(self.departures[stop])],
                "realtimeDataUpdatedAt": int(time.time())}
        return 404, {"message": "not found"}
