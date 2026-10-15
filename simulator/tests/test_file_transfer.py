#!/usr/bin/env python3
"""File Transfer serves the SD card and SPIFFS to a browser that has the
password the device shows.

The simulator opens the app on a scratch copy of simulator/sdcard and a
scratch folder standing in for SPIFFS, and its server listens on a free
port on 127.0.0.1. The simulator prints the password, as a device never
does, so the test can sign in. Over HTTP it checks:

- the page is served to anyone, everything else only once signed in, and a
  401 never asks the browser for Basic authentication;
- signing in wants the password in either case, with or without its dash,
  and the X-Thistle-Request header; it sets an HttpOnly, SameSite=Strict
  cookie, and signing out ends it;
- listing the device, the SD card and a folder;
- an upload lands byte for byte, will not replace a file unless asked, and
  needs X-Thistle-Request with the cookie but not with Basic authentication;
- a download comes back byte for byte, with its length, and as an
  attachment when asked; an .html file is shown as text in a sandbox;
- folders are made, files and folders renamed, and removed, a folder only
  when empty;
- no path gets out of /sdcard and /spiffs, however it is written;
- SPIFFS takes files, and refuses names longer than it can hold;
- five wrong passwords in a row lock sign-ins, Basic ones too.
"""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import quote

SIM_DIR = Path(__file__).resolve().parent.parent
CHANGE = {"X-Thistle-Request": "1"}


class Sim:
    """The simulator with File Transfer open, its output read as it comes."""

    def __init__(self, simulator: Path, card: Path, spiffs: Path) -> None:
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs),
                   THISTLE_SIM_HTTPD_PORT="0")
        self.proc = subprocess.Popen(
            [str(simulator), "--headless", "--device", "tdeck-max", "--timeout", "120000",
             "--launch-app", "com.thistle.filexfer"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines: list[str] = []
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()
        self.password = self.wait_for(r"\[filexfer\] password (\S+)")
        self.port = int(self.wait_for(r"\[sim_httpd\] listening on http://127\.0\.0\.1:(\d+)"))

    def _read(self) -> None:
        assert self.proc.stdout
        for line in self.proc.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))

    def output(self) -> str:
        with self.lock:
            return "\n".join(self.lines)

    def wait_for(self, pattern: str, timeout: float = 20.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = re.search(pattern, self.output())
            if m:
                return m.group(1)
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        tail = "\n".join(self.output().splitlines()[-20:])
        raise AssertionError(f"no {pattern!r} from the simulator; it ended:\n{tail}")

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class Client:
    def __init__(self, port: int) -> None:
        self.port = port
        self.cookie: str | None = None

    def request(self, method: str, path: str, body: bytes | str | None = None,
                headers: dict | None = None, cookie: bool = True):
        h = dict(headers or {})
        if cookie and self.cookie:
            h["Cookie"] = self.cookie
        if isinstance(body, str):
            body = body.encode()
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, data


def expect(condition: bool, what: str) -> None:
    if not condition:
        raise AssertionError(what)


def expect_status(got: tuple, want: int, what: str) -> None:
    status, _, body = got
    expect(status == want, f"{what}: {status}, not {want} ({body[:200]!r})")


def q(path: str) -> str:
    return quote(path, safe="")


def test_signing_in(c: Client, password: str) -> None:
    status, headers, body = c.request("GET", "/")
    expect(status == 200 and b"<title>" in body, "the page is served")
    expect("default-src 'self'" in headers.get("content-security-policy", ""), "the page has a CSP")

    status, headers, _ = c.request("GET", "/api/info")
    expect(status == 401, f"info before signing in: {status}")
    expect("www-authenticate" not in headers, "a 401 never asks for Basic")

    expect_status(c.request("POST", "/api/login", password), 403, "a login without the header")
    expect_status(c.request("POST", "/api/login", "WRONG-PASS", CHANGE), 401, "a wrong password")

    loose = password.replace("-", "").lower()
    status, headers, _ = c.request("POST", "/api/login", f"  {loose}\n", CHANGE)
    expect(status == 200, f"the password, lower case, no dash: {status}")
    cookie = headers.get("set-cookie", "")
    expect("HttpOnly" in cookie and "SameSite=Strict" in cookie, f"the cookie: {cookie!r}")
    c.cookie = cookie.split(";")[0]


def test_listing(c: Client) -> None:
    status, _, body = c.request("GET", "/api/info")
    expect(status == 200, f"info: {status}")
    info = json.loads(body)
    expect(info["hostname"] == "thistledev.local", f"hostname: {info['hostname']}")
    names = {r["name"]: r for r in info["roots"]}
    expect(names["sdcard"]["mounted"] and names["spiffs"]["mounted"], f"roots: {names}")

    status, _, body = c.request("GET", "/api/list?path=" + q("/"))
    top = {e["name"] for e in json.loads(body)["entries"]}
    expect(top == {"sdcard", "spiffs"}, f"the device: {top}")

    status, _, body = c.request("GET", "/api/list?path=" + q("/sdcard"))
    listing = json.loads(body)
    expect(listing["path"] == "/sdcard", f"path: {listing['path']}")
    config = next((e for e in listing["entries"] if e["name"] == "config"), None)
    expect(config is not None and config["dir"], "the card's config folder is listed")

    status, _, body = c.request("GET", "/api/list?path=" + q("/sdcard/config"))
    sysjson = next((e for e in json.loads(body)["entries"] if e["name"] == "system.json"), None)
    expect(sysjson is not None and not sysjson["dir"] and sysjson["size"] > 0,
           "system.json is listed with its size")

    expect_status(c.request("GET", "/api/list?path=" + q("/sdcard/none")), 404, "a missing folder")
    expect_status(c.request("GET", "/api/list?path=" + q("/sdcard/config/system.json")), 400,
                  "listing a file")


def test_upload_download(c: Client, card: Path) -> None:
    data = bytes(range(256)) * 1200 + b"tail"   # 300 KB, every byte value
    path = "/sdcard/notes/xfer test ü.bin"
    url = "/api/file?path=" + q(path)

    expect_status(c.request("PUT", url, data), 403, "an upload with the cookie but no header")
    expect_status(c.request("PUT", url, data, CHANGE), 201, "an upload")
    on_card = card / "notes" / "xfer test ü.bin"
    expect(on_card.read_bytes() == data, "the upload is on the card byte for byte")
    expect(not (card / ".xfer-upload.tmp").exists(), "no temporary file is left")

    expect_status(c.request("PUT", url, b"other", CHANGE), 409, "replacing without asking")
    expect(on_card.read_bytes() == data, "a refused upload leaves the file")
    expect_status(c.request("PUT", url + "&overwrite=1", b"replaced", CHANGE), 200, "replacing")
    expect(on_card.read_bytes() == b"replaced", "the file is replaced")
    expect_status(c.request("PUT", url + "&overwrite=1", data, CHANGE), 200, "replacing again")

    status, headers, body = c.request("GET", url)
    expect(status == 200 and body == data, f"the download, byte for byte ({status}, {len(body)})")
    expect(headers.get("content-length") == str(len(data)), "the download has its length")
    expect(headers.get("content-disposition") == "inline", "shown, unless asked to save")

    status, headers, body = c.request("GET", url + "&download=1")
    disposition = headers.get("content-disposition", "")
    expect(status == 200 and body == data, "the download, as an attachment")
    expect(disposition.startswith("attachment;") and "filename*=UTF-8''xfer%20test%20%C3%BC.bin"
           in disposition, f"saved under its name: {disposition!r}")

    page = "/sdcard/notes/page.html"
    expect_status(c.request("PUT", "/api/file?path=" + q(page), b"<script>alert(1)</script>",
                            CHANGE), 201, "an .html file")
    status, headers, _ = c.request("GET", "/api/file?path=" + q(page))
    expect(headers.get("content-type", "").startswith("text/plain"), "html is shown as text")
    expect("sandbox" in headers.get("content-security-policy", ""), "files come in a sandbox")
    expect(headers.get("x-content-type-options") == "nosniff", "and are not sniffed")

    expect_status(c.request("PUT", "/api/file?path=" + q("/sdcard/nofolder/a.txt"), b"x", CHANGE),
                  404, "an upload into a missing folder")


def test_folders(c: Client, card: Path) -> None:
    expect_status(c.request("POST", "/api/mkdir?path=" + q("/sdcard/xfer-dir"), None, CHANGE), 201,
                  "making a folder")
    expect((card / "xfer-dir").is_dir(), "the folder is on the card")
    expect_status(c.request("POST", "/api/mkdir?path=" + q("/sdcard/xfer-dir"), None, CHANGE), 409,
                  "making it again")

    expect_status(c.request("PUT", "/api/file?path=" + q("/sdcard/xfer-dir/a.txt"), b"a", CHANGE),
                  201, "a file in it")
    status, _, body = c.request("POST", "/api/rename?path=" + q("/sdcard/xfer-dir/a.txt") +
                                "&name=" + q("b.txt"), None, CHANGE)
    expect(status == 200 and b'"/sdcard/xfer-dir/b.txt"' in body, f"renaming: {status} {body!r}")
    expect((card / "xfer-dir" / "b.txt").exists() and not (card / "xfer-dir" / "a.txt").exists(),
           "renamed on the card")
    expect_status(c.request("POST", "/api/rename?path=" + q("/sdcard/xfer-dir/b.txt") + "&name=" +
                            q("../escaped.txt"), None, CHANGE), 400, "renaming out of the folder")

    expect_status(c.request("DELETE", "/api/file?path=" + q("/sdcard/xfer-dir"), None, CHANGE), 409,
                  "removing a folder with a file in it")
    expect_status(c.request("DELETE", "/api/file?path=" + q("/sdcard/xfer-dir/b.txt"), None, CHANGE),
                  200, "removing the file")
    expect_status(c.request("DELETE", "/api/file?path=" + q("/sdcard/xfer-dir"), None, CHANGE), 200,
                  "removing the empty folder")
    expect(not (card / "xfer-dir").exists(), "the folder is gone from the card")
    expect_status(c.request("DELETE", "/api/file?path=" + q("/sdcard"), None, CHANGE), 400,
                  "removing a root")


def test_paths_stay_inside(c: Client) -> None:
    for raw in ("/etc/passwd", "/sdcard/../../etc/passwd", "/sdcard/..", "sdcard/config",
                "/spiffs/../sdcard", "/sdcard/a\\..\\b"):
        expect_status(c.request("GET", "/api/file?path=" + q(raw)), 400, f"reading {raw!r}")
    # Encoded twice, the dots arrive as "%2e%2e" and are a name, not a climb.
    status, _, _ = c.request("GET", "/api/file?path=" + q("/sdcard/%2e%2e/etc"))
    expect(status == 404, f"a name that only looks like a climb: {status}")
    expect_status(c.request("GET", "/api/file?path=/sdcard/%2e%2e/%2e%2e/etc/passwd"), 400,
                  "a climb encoded once")
    expect_status(c.request("GET", "/api/file"), 400, "no path at all")
    expect_status(c.request("PUT", "/api/file?path=" + q("/sdcard/.xfer-upload.tmp"), b"x", CHANGE),
                  400, "the temporary file's name")


def test_spiffs(c: Client, spiffs: Path) -> None:
    expect_status(c.request("PUT", "/api/file?path=" + q("/spiffs/x.json"), b"{}", CHANGE), 201,
                  "a file on SPIFFS")
    expect((spiffs / "x.json").read_bytes() == b"{}", "the file is in SPIFFS's folder")
    long = "/spiffs/" + "a" * 31
    expect_status(c.request("PUT", "/api/file?path=" + q(long), b"x", CHANGE), 400,
                  "a name longer than SPIFFS holds")


def test_basic(c: Client, password: str) -> None:
    good = "Basic " + base64.b64encode(f"me:{password}".encode()).decode()
    status, _, _ = c.request("GET", "/api/list?path=" + q("/sdcard"),
                             headers={"Authorization": good}, cookie=False)
    expect(status == 200, f"Basic with the password: {status}")
    expect_status(c.request("PUT", "/api/file?path=" + q("/sdcard/basic.txt"), b"by curl",
                            {"Authorization": good}, cookie=False), 201,
                  "an upload with Basic, no header needed")
    bad = "Basic " + base64.b64encode(b":WRONGPWD").decode()
    expect_status(c.request("GET", "/api/info", headers={"Authorization": bad}, cookie=False), 401,
                  "Basic with a wrong password")


def test_logout_and_lock(c: Client, password: str) -> None:
    expect_status(c.request("POST", "/api/logout", None, CHANGE), 200, "signing out")
    expect_status(c.request("GET", "/api/info"), 401, "the cookie after signing out")

    # One wrong one went by in test_signing_in and one in test_basic; a right
    # one between them started the count again, so four more lock it.
    for i in range(3):
        expect_status(c.request("POST", "/api/login", "WRONGPWD", CHANGE), 401, f"wrong #{i + 3}")
    expect_status(c.request("POST", "/api/login", "WRONGPWD", CHANGE), 401,
                  "the fifth, which locks")
    status, headers, body = c.request("POST", "/api/login", password, CHANGE)
    expect(status == 429, f"the right password while locked: {status} {body!r}")
    expect(int(headers.get("retry-after", "0")) > 0, "with Retry-After")
    good = "Basic " + base64.b64encode(f":{password}".encode()).decode()
    expect_status(c.request("GET", "/api/info", headers={"Authorization": good}, cookie=False), 429,
                  "Basic while locked")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sim", type=Path, default=SIM_DIR / "build" / "thistle_sim")
    parser.add_argument("--keep", action="store_true", help="keep the scratch card")
    args = parser.parse_args()
    if not args.sim.exists():
        print(f"no simulator at {args.sim}: build it first", file=sys.stderr)
        return 2

    root = Path(tempfile.mkdtemp(prefix="thistle-xfer-"))
    card = root / "sdcard"
    spiffs = root / "spiffs"
    shutil.copytree(SIM_DIR / "sdcard", card)
    (card / "notes").mkdir(exist_ok=True)
    spiffs.mkdir()

    sim = Sim(args.sim, card, spiffs)
    failed = 0
    try:
        c = Client(sim.port)
        tests = [
            ("signing in", lambda: test_signing_in(c, sim.password)),
            ("listing", lambda: test_listing(c)),
            ("uploads and downloads", lambda: test_upload_download(c, card)),
            ("folders", lambda: test_folders(c, card)),
            ("paths stay inside", lambda: test_paths_stay_inside(c)),
            ("SPIFFS", lambda: test_spiffs(c, spiffs)),
            ("Basic authentication", lambda: test_basic(c, sim.password)),
            ("signing out and the lock", lambda: test_logout_and_lock(c, sim.password)),
        ]
        for name, test in tests:
            try:
                test()
                print(f"ok    {name}")
            except AssertionError as e:
                failed += 1
                print(f"FAIL  {name}: {e}")
    finally:
        sim.stop()
        if args.keep:
            print(f"kept {root}")
        else:
            shutil.rmtree(root, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
