#!/usr/bin/env python3
"""AssistNow on the simulated T-Deck Max (gps_assist.rs): with a token on
the card, the kernel registers the receiver with Thingstream, fetches a
week of orbit predictions with the chipcode it is given, keeps them on
SPIFFS, and hands them to the receiver; kept orbits need no request; and
without a token nothing is asked of anyone. Each start is reported on the
card (gps_report.rs), unless switched off.

A local HTTP server stands in for Thingstream's registration and for the
AssistNow service; the card's config/assistnow.json points the kernel at
the first, and the registration's answer at the second. The simulated
receiver has no fix (a scenario file), so it takes what it is handed, and
says so: "[sim_gps] assistance: ...".

Every case runs on a scratch copy of simulator/sdcard and an empty SPIFFS.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable, Optional
from urllib.parse import parse_qs, urlsplit

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"
TOKEN = "test-profile-token"
CHIPCODE = "c2ltY2hpcA+/="
NO_FIX = {"gps": {"latitude": 59.33, "longitude": 18.07, "altitude_m": 20,
                  "satellites": 0, "fix_valid": False}}
FIX = {"gps": {"latitude": 59.33, "longitude": 18.07, "altitude_m": 20,
               "satellites": 8, "fix_valid": True}}


def ubx(cls: int, msg_id: int, payload: bytes) -> bytes:
    body = bytes([cls, msg_id]) + len(payload).to_bytes(2, "little") + payload
    a = b = 0
    for x in body:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return b"\xb5\x62" + body + bytes([a, b])


def orbits() -> bytes:
    """Three GPS almanacs and today's predictions for three satellites, as
    the service sends them."""
    today = time.gmtime()
    date = bytes([today.tm_year - 2000, today.tm_mon, today.tm_mday])
    almanacs = b"".join(ubx(0x13, 0x00, bytes([2, 0, sv, 0]) + bytes(32)) for sv in (1, 2, 3))
    predictions = b"".join(ubx(0x13, 0x20, bytes([0, 0, sv, 0]) + date + bytes(69)) for sv in (1, 2, 3))
    return almanacs + predictions


class Service:
    """Thingstream's registration and the AssistNow service: what they
    answer, and every request made of them."""

    def __init__(self, fetch_status: int = 200) -> None:
        self.fetch_status = fetch_status
        self.data = orbits()
        self.registrations: list[dict] = []
        self.fetches: list[str] = []
        service = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"null")
                service.registrations.append({"path": self.path, "type": self.headers.get("Content-Type"),
                                              "body": body})
                # allowedData as the service sends it, one string; u-blox's
                # guide shows a list.
                self.answer(200, json.dumps({
                    "allowedData": "ualm, uporb_1, uporb_3, uporb_7, uporb_14, ulorb_l1, utime, ukion, usvht",
                    "chipcode": CHIPCODE,
                    "serviceUrl": f"{service.base}/GetAssistNowData.ashx",
                }).encode())

            def do_GET(self) -> None:  # noqa: N802
                service.fetches.append(self.path)
                if service.fetch_status == 200:
                    self.answer(200, service.data)
                else:
                    self.answer(service.fetch_status,
                                b"Access is blocked - Predictive Orbits usage limit has been reached")

            def answer(self, status: int, body: bytes) -> None:
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def with_token(card: Path, spiffs: Path, service: Service) -> None:
    (card / "config/assistnow.json").write_text(
        json.dumps({"ztp_token": TOKEN, "ztp_url": f"{service.base}/ztp"}))


def fetched_for(gnss: str) -> Callable[[Path, Path, Service], None]:
    """As a fetch an hour ago left it, for these constellations."""
    def setup(card: Path, spiffs: Path, service: Service) -> None:
        with_token(card, spiffs, service)
        (spiffs / "assistnow.json").write_text(json.dumps({
            "chipcode": CHIPCODE, "service_url": f"{service.base}/GetAssistNowData.ashx",
            "days": 7, "almanac": True, "fetched": int(time.time()) - 3600, "gnss": gnss,
        }))
        (spiffs / "assistnow.ubx").write_bytes(service.data)
    return setup


def registered_only(card: Path, spiffs: Path, service: Service) -> None:
    with_token(card, spiffs, service)
    (spiffs / "assistnow.json").write_text(json.dumps({
        "chipcode": CHIPCODE, "service_url": f"{service.base}/GetAssistNowData.ashx",
        "days": 7, "almanac": True, "fetched": 0,
    }))


def report_off(card: Path, spiffs: Path, service: Service) -> None:
    (card / "config/assistnow.json").write_text(json.dumps({"report": False}))


def many_reports(card: Path, spiffs: Path, service: Service) -> None:
    starts = card / "gps/starts"
    starts.mkdir(parents=True)
    for n in range(1, 26):
        (starts / f"{n:04d}.jsonl").write_text("{}\n")
    (starts / "notes.txt").write_text("not a report")


def reports(card: Path) -> list[Path]:
    return sorted((card / "gps/starts").glob("*.jsonl"))


def report_lines(card: Path) -> tuple[Optional[str], list[dict]]:
    found = reports(card)
    if len(found) != 1:
        return f"{len(found)} reports: {[p.name for p in found]}", []
    name = found[0].name
    if not (name.startswith("0001-") and len(name) == len("0001-20261021-113503.jsonl")):
        return f"named {name}", []
    return None, [json.loads(line) for line in found[0].read_text().splitlines()]


def reported_without_a_fix(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    problem, lines = report_lines(card)
    if problem:
        return problem
    types = [line["type"] for line in lines]
    if types[0] != "start" or "handed" not in types or "sats" not in types or "fix" in types:
        return f"lines {types}"
    start = lines[0]
    if start["columns"][:2] != ["sv", "dBHz"] or not start["utc"]:
        return f"start {start}"
    handed = next(line for line in lines if line["type"] == "handed")
    if handed["time_acc_ms"] is None or handed["position_acc_m"] is not None:
        return f"handed {handed}"
    sats = next(line for line in lines if line["type"] == "sats")
    if len(sats["sats"]) != 4 or sats["used"] != 0 or sats["heard"] != 4:
        return f"sats {sats}"
    if sats["sats"][0] != ["G01", 25, 10, 0, 4, 0, 1, "pred", "alm+pred"]:
        return f"a satellite reads {sats['sats'][0]}"
    text = (reports(card)[0]).read_text()
    if "59.33" in text or "18.07" in text:
        return "the position is in the report"
    return None


def reported_with_a_fix(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    problem, lines = report_lines(card)
    if problem:
        return problem
    fixes = [line for line in lines if line["type"] == "fix"]
    if len(fixes) != 1 or fixes[0]["in_use"] != 8:
        return f"fix lines {fixes}"
    sats = [line for line in lines if line["type"] == "sats"]
    if not sats or sats[0]["used"] != 8:
        return f"sats {sats[:1]}"
    return None


def reported_nothing(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    return f"reports {reports(card)}" if reports(card) else None


def the_oldest_reports_went(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    names = [p.name for p in reports(card)]
    if len(names) != 20 or names[0] != "0007.jsonl" or not names[-1].startswith("0026-"):
        return f"left {names}"
    if not (card / "gps/starts/notes.txt").exists():
        return "removed what was not a report"
    return None


def registration_sent_the_receiver(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    if len(service.registrations) != 1:
        return f"{len(service.registrations)} registrations"
    reg = service.registrations[0]
    if reg["path"] != "/ztp" or reg["type"] != "application/json":
        return f"registered at {reg['path']} as {reg['type']}"
    body = reg["body"]
    if body.get("token") != TOKEN:
        return "the registration did not carry the token"
    messages = body.get("messages", {})
    if not messages.get("UBX-SEC-UNIQID", "").startswith("B5622703"):
        return f"UBX-SEC-UNIQID: {messages.get('UBX-SEC-UNIQID')!r}"
    if not messages.get("UBX-MON-VER", "").startswith("B5620A04"):
        return f"UBX-MON-VER: {messages.get('UBX-MON-VER', '')[:16]!r}"
    return fetched_with_the_chipcode(service, spiffs, card)


def fetched_with_the_chipcode(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    if len(service.fetches) != 1:
        return f"{len(service.fetches)} fetches"
    url = urlsplit(service.fetches[0])
    query = parse_qs(url.query)
    if url.path != "/GetAssistNowData.ashx":
        return f"fetched {url.path}"
    if query.get("chipcode") != [CHIPCODE] or "%2B%2F%3D" not in url.query:
        return f"the chipcode went as {url.query}"
    if query.get("gnss") != ["gps,gal,glo"] or query.get("data") != ["uporb_7,ualm"]:
        return f"asked for {url.query}"
    if (spiffs / "assistnow.ubx").read_bytes() != service.data:
        return "assistnow.ubx is not what the service sent"
    kept = json.loads((spiffs / "assistnow.json").read_text())
    if kept.get("chipcode") != CHIPCODE or not kept.get("fetched"):
        return f"assistnow.json holds {kept}"
    return None


def asked_nothing(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    if service.registrations or service.fetches:
        return f"asked {len(service.registrations)} registrations, {service.fetches}"
    return None


def one_fetch_only(service: Service, spiffs: Path, card: Path) -> Optional[str]:
    if len(service.fetches) != 1:
        return f"{len(service.fetches)} fetches: a refusal is not asked again at once"
    if (spiffs / "assistnow.ubx").exists():
        return "a refusal was kept as data"
    # Kept, so a restart does not ask again either.
    kept = json.loads((spiffs / "assistnow.json").read_text())
    if kept.get("retry_at", 0) <= time.time() + 3600:
        return f"the wait was not kept: {kept}"
    return None


@dataclass(frozen=True)
class Case:
    name: str
    setup: Optional[Callable[[Path, Path, Service], None]]
    expected: tuple[str, ...]
    check: Callable[[Service, Path, Path], Optional[str]]
    fetch_status: int = 200
    absent: tuple[str, ...] = ()
    timeout_ms: int = 8000
    scenario: dict = None  # NO_FIX unless given


CASES = (
    Case("registers-fetches-and-hands-the-orbits-over", with_token,
         expected=("[sim_gps] assistance: time given, position none, 0 messages",
                   "this receiver is registered; orbits come 7 days at a time",
                   "7 days of orbits fetched: 6 messages, 384 bytes",
                   "[sim_gps] assistance: time given, position none, 6 messages"),
         check=registration_sent_the_receiver),
    Case("kept-orbits-need-no-request", fetched_for("gps,gal,glo"),
         expected=("orbits from ", "[sim_gps] assistance: time given, position none, 6 messages",
                   "handed to the receiver: the time to "),
         check=asked_nothing),
    # Before GLONASS was asked for too.
    Case("orbits-kept-for-other-constellations-are-fetched-again", fetched_for("gps,gal"),
         expected=("orbits from ", "7 days of orbits fetched: 6 messages, 384 bytes"),
         check=fetched_with_the_chipcode),
    Case("no-token-asks-nothing", None,
         expected=("no token: GPS starts without orbits",
                   "[sim_gps] assistance: time given, position none, 0 messages"),
         check=asked_nothing),
    Case("a-refused-fetch-waits", registered_only, fetch_status=403,
         expected=("fetch failed, HTTP 403: Access is blocked - Predictive Orbits usage limit",),
         absent=("orbits fetched",), check=one_fetch_only),
    Case("each-start-is-reported-on-the-card", None,
         expected=("reporting this GPS start in gps/starts/0001-",),
         check=reported_without_a_fix),
    Case("a-fix-is-reported", None, scenario=FIX,
         expected=("reporting this GPS start in gps/starts/0001-",),
         check=reported_with_a_fix),
    Case("the-oldest-reports-make-room", many_reports,
         expected=("reporting this GPS start in gps/starts/0026-",),
         check=the_oldest_reports_went),
    Case("reports-can-be-switched-off", report_off,
         expected=("no token: GPS starts without orbits",),
         absent=("reporting this GPS start",), check=reported_nothing),
)


def in_order(log: str, expected: tuple[str, ...]) -> Optional[str]:
    at = 0
    for line in expected:
        found = log.find(line, at)
        if found < 0:
            return line
        at = found + len(line)
    return None


def run_case(simulator: Path, case: Case) -> None:
    service = Service(case.fetch_status)
    try:
        with tempfile.TemporaryDirectory(prefix="thistle-assistnow-") as tmp:
            card = Path(tmp) / "card"
            spiffs = Path(tmp) / "spiffs"
            shutil.copytree(SDCARD, card)
            spiffs.mkdir()
            (card / "config/assistnow.json").unlink(missing_ok=True)
            if case.setup:
                case.setup(card, spiffs, service)
            scenario = Path(tmp) / "no_fix.json"
            scenario.write_text(json.dumps(case.scenario or NO_FIX))
            env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs))
            command = [str(simulator), "--headless", "--device", "tdeck-max",
                       "--scenario", str(scenario), "--timeout", str(case.timeout_ms)]
            run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                                 check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
            problem = case.check(service, spiffs, card)
    finally:
        service.close()

    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    if problem:
        raise AssertionError(problem)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== AssistNow ===")
    failures = 0
    for case in cases:
        try:
            run_case(simulator, case)
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {case.name}: {exc}")
        else:
            print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(cases)} AssistNow tests failed")
        return 1
    print(f"All {len(cases)} AssistNow tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
