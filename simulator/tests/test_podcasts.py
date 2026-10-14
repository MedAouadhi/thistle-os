#!/usr/bin/env python3
"""The Podcasts app browses PodcastIndex, plays an episode, and saves one to
the SD card to play with no network.

A local server stands in for PodcastIndex and for a podcast host. The host
serves an MP3 the test makes, a minute of silent MPEG-1 Layer III frames
behind an ID3 tag, from behind a redirect, as podcast hosts' tracking links
are, and honours Range requests. Each run opens the app on a scratch copy of
simulator/sdcard whose config/podcasts.json points it at the server, taps
through it, and checks:

- every request to the API is signed: X-Auth-Key, a current X-Auth-Date and
  Authorization = sha1(key + secret + date), with a User-Agent;
- Trending, a podcast and an episode open, and Play streams the episode
  through the redirect, past its ID3 tag, to the audio driver;
- Save writes the episode to podcasts/<feed>/<episode>.mp3, byte for byte,
  and the library records it;
- with the server gone, the saved episode plays from the card;
- a search sends the query and lists what it finds;
- a podcast starred is in the library, starred;
- skipping ahead asks the host for the byte that far into the audio;
- an episode goes on playing, to its end, after the app is left.

The real PodcastIndex is never asked: the card's key replaces any built in.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

SIM_DIR = Path(__file__).resolve().parent.parent
KEY = "TESTKEYTESTKEYTESTKE"
SECRET = "test-secret|with^marks"
FEED_ID = 7001
EPISODE_ID = 61668942832  # past 2^32, as PodcastIndex's are
M4A_ID = 80002
SHORT_ID = 80003
ID3_PADDING = 2000

# Where things are on the T-Deck Max's 240 x 320 screen; the status bar is
# 24 px, a page's title bar 30.
HOME_TRENDING = "100,100"
HOME_SAVED = "100,224"
FIRST_ROW = "100,80"           # a list's first two-line row
PODCAST_FIRST_EPISODE = "100,145"
PODCAST_THIRD_EPISODE = "100,252"
EPISODE_PLAY = "55,148"
EPISODE_SAVE = "170,148"
TITLE_STAR = "220,39"
PLAYER_AHEAD = "197,236"       # +30
HOME_SEARCH = "100,69"


def silent_mp3(seconds: int) -> bytes:
    """MPEG-1 Layer III, 128 kbps, 44.1 kHz, mono: frames whose side
    information is all zeros, which decode to silence, behind an ID3v2 tag
    of padding."""
    header = bytes((0xFF, 0xFB, 0x90, 0xC0))
    frame = header + bytes(417 - len(header))
    frames = seconds * 44100 // 1152
    size = ID3_PADDING
    syncsafe = bytes(((size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F))
    tag = b"ID3\x03\x00\x00" + syncsafe + bytes(size)
    return tag + frame * frames


AUDIO = silent_mp3(60)
SHORT_AUDIO = silent_mp3(8)
FILES = {"/audio/ep1.mp3": AUDIO, "/audio/short.mp3": SHORT_AUDIO}


class FakeIndex:
    """PodcastIndex's API under /api/1.0, and a podcast host beside it."""

    def __init__(self) -> None:
        self.requests: list[dict] = []
        self.lock = threading.Lock()
        index = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:  # noqa: N802 (http.server API)
                headers = {k.lower(): v for k, v in self.headers.items()}
                with index.lock:
                    index.requests.append({"path": self.path, "headers": headers})
                index.answer(self, headers)

            def log_message(self, *args) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def take(self) -> list[dict]:
        with self.lock:
            taken, self.requests = self.requests, []
        return taken

    def feed(self) -> dict:
        return {"id": FEED_ID, "title": "Test Talk", "author": "Thistle Tester",
                "ownerName": "Thistle", "description": "<p>A podcast for tests.</p>",
                "language": "en", "episodeCount": 2, "categories": {"102": "Technology"},
                "newestItemPubdate": int(time.time()) - 3600}

    def episodes(self) -> list[dict]:
        now = int(time.time())
        return [
            {"id": EPISODE_ID, "title": "First Light", "feedId": FEED_ID,
             "enclosureUrl": f"{self.base}/redirect/ep1.mp3", "enclosureType": "audio/mpeg",
             "enclosureLength": len(AUDIO), "duration": 60, "datePublished": now - 3600,
             "description": "<p>The first one, with a <b>bold</b> word.</p>"},
            {"id": M4A_ID, "title": "Second Light", "feedId": FEED_ID,
             "enclosureUrl": f"{self.base}/audio/ep2.m4a", "enclosureType": "audio/x-m4a",
             "enclosureLength": 1000, "duration": 30, "datePublished": now - 90000,
             "description": "AAC, which the app cannot play."},
            {"id": SHORT_ID, "title": "Short Light", "feedId": FEED_ID,
             "enclosureUrl": f"{self.base}/audio/short.mp3", "enclosureType": "audio/mpeg",
             "enclosureLength": len(SHORT_AUDIO), "duration": 8, "datePublished": now - 200000,
             "description": "Eight seconds."},
        ]

    def answer(self, handler: BaseHTTPRequestHandler, headers: dict) -> None:
        url = urlsplit(handler.path)
        query = parse_qs(url.query)
        if url.path == "/redirect/ep1.mp3":
            handler.send_response(302)
            handler.send_header("Location", "/audio/ep1.mp3")
            handler.send_header("Content-Length", "0")
            handler.end_headers()
            return
        if url.path in FILES:
            audio = FILES[url.path]
            start = 0
            match = re.match(r"bytes=(\d+)-", headers.get("range", ""))
            if match:
                start = int(match.group(1))
            body = audio[start:]
            handler.send_response(206 if match else 200)
            handler.send_header("Content-Type", "audio/mpeg")
            handler.send_header("Content-Length", str(len(body)))
            if match:
                handler.send_header("Content-Range", f"bytes {start}-{len(audio) - 1}/{len(audio)}")
            handler.end_headers()
            try:
                handler.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # the player let go of it
            return

        if url.path == "/api/1.0/podcasts/trending":
            payload = {"status": "true", "feeds": [self.feed()], "count": 1}
        elif url.path == "/api/1.0/episodes/byfeedid":
            payload = {"status": "true", "items": self.episodes(), "count": 3}
        elif url.path == "/api/1.0/search/byterm":
            found = [self.feed()] if "test" in query.get("q", [""])[0].lower() else []
            payload = {"status": "true", "feeds": found, "count": len(found)}
        else:
            payload = {"status": "false", "description": "Not here"}
        body = json.dumps(payload).encode()
        handler.send_response(200 if payload["status"] == "true" else 404)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def __enter__(self) -> "FakeIndex":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()


class Card:
    """A scratch SD card for the simulator, with nothing of the app's on it
    but a key pointing at the fake PodcastIndex."""

    def __init__(self, root: Path, name: str, base: str) -> None:
        self.path = root / name
        shutil.copytree(SIM_DIR / "sdcard", self.path)
        shutil.rmtree(self.path / "data" / "podcasts", ignore_errors=True)
        shutil.rmtree(self.path / "podcasts", ignore_errors=True)
        (self.path / "config" / "podcasts.json").write_text(json.dumps(
            {"api_key": KEY, "api_secret": SECRET, "api_url": f"{base}/api/1.0"}))

    def library(self) -> dict:
        path = self.path / "data" / "podcasts" / "library.json"
        return json.loads(path.read_text()) if path.exists() else {"feeds": [], "episodes": []}

    def episode(self, episode_id: int) -> dict | None:
        return next((e for e in self.library()["episodes"] if e["id"] == episode_id), None)

    def feed(self, feed_id: int) -> dict | None:
        return next((f for f in self.library()["feeds"] if f["id"] == feed_id), None)

    def saved_file(self) -> Path:
        return self.path / "podcasts" / str(FEED_ID) / f"{EPISODE_ID}.mp3"


def run_sim(simulator: Path, card: Card, taps: tuple[str, ...] = (), text: str | None = None,
            timeout_ms: int | None = None, screenshot: Path | None = None) -> str:
    timeout_ms = timeout_ms or 2250 + 1500 * len(taps) + 2500
    command = [str(simulator), "--headless", "--device", "tdeck-max",
               "--timeout", str(timeout_ms), "--report-offscreen",
               "--launch-app", "com.thistle.podcasts"]
    for tap in taps:
        command.extend(("--tap", tap))
    if text is not None:
        command.extend(("--type", text))
    if screenshot:
        command.extend(("--screenshot", str(screenshot)))
    env = dict(os.environ, THISTLE_SIM_SDCARD=str(card.path))
    run = subprocess.run(command, env=env, text=True, capture_output=True,
                         check=False, timeout=120)
    out = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}: {out.strip()[-500:]!r}")
    offscreen = [line for line in run.stdout.splitlines() if line.startswith("Off screen: ")]
    if offscreen:
        raise AssertionError("drawn off the screen: " + "; ".join(offscreen))
    return out


def expect(condition: bool, what: str) -> None:
    if not condition:
        raise AssertionError(what)


def expect_in(needle: str, out: str) -> None:
    if needle not in out:
        tail = "\n".join(out.splitlines()[-25:])
        raise AssertionError(f"{needle!r} not in the simulator's output; it ended:\n{tail}")


def check_signed(requests: list[dict]) -> None:
    api = [r for r in requests if r["path"].startswith("/api/")]
    expect(api, "no request reached the API")
    for request in api:
        h = request["headers"]
        expect(h.get("x-auth-key") == KEY, f"X-Auth-Key: {h.get('x-auth-key')!r}")
        date = h.get("x-auth-date", "")
        expect(date.isdigit() and abs(int(date) - time.time()) < 300, f"X-Auth-Date: {date!r}")
        want = hashlib.sha1((KEY + SECRET + date).encode()).hexdigest()
        expect(h.get("authorization") == want, f"Authorization: {h.get('authorization')!r}, not {want}")
        expect(h.get("user-agent", "").startswith("ThistleOS-Podcasts/"),
               f"User-Agent: {h.get('user-agent')!r}")


def test_browse_and_play(simulator: Path, index: FakeIndex, root: Path) -> None:
    card = Card(root, "play", index.base)
    index.take()
    out = run_sim(simulator, card,
                  (HOME_TRENDING, FIRST_ROW, PODCAST_FIRST_EPISODE, EPISODE_PLAY),
                  timeout_ms=2250 + 1500 * 4 + 5000, screenshot=root / "play.ppm")
    requests = index.take()
    check_signed(requests)
    paths = [urlsplit(r["path"]).path for r in requests]
    expect("/api/1.0/podcasts/trending" in paths, f"Trending not asked for: {paths}")
    expect("/api/1.0/episodes/byfeedid" in paths, f"episodes not asked for: {paths}")
    expect_in("1 podcasts", out)
    expect_in("3 episodes", out)
    expect_in(f"playing {EPISODE_ID} from 0 s, streamed", out)
    expect("/redirect/ep1.mp3" in paths and "/audio/ep1.mp3" in paths,
           f"the episode was not fetched through its redirect: {paths}")
    expect_in(f"skipping a {ID3_PADDING + 10} byte ID3 tag", out)
    expect_in("[sim_audio] configure sample_rate=44100 bits=16 channels=1", out)
    expect_in("[sim_audio] playing", out)

    feed = card.feed(FEED_ID)
    expect(feed is not None and feed["viewed_at"] > 0, f"the podcast was not recorded as viewed: {feed}")
    episode = card.episode(EPISODE_ID)
    expect(episode is not None and episode["played_at"] > 0 and episode["viewed_at"] > 0,
           f"the episode was not recorded as viewed and played: {episode}")
    expect(episode["title"] == "First Light", f"title kept: {episode['title']!r}")


def test_save_then_play_offline(simulator: Path, root: Path) -> None:
    with FakeIndex() as index:
        card = Card(root, "save", index.base)
        run_sim(simulator, card, (HOME_TRENDING, FIRST_ROW, PODCAST_FIRST_EPISODE, EPISODE_SAVE),
                timeout_ms=2250 + 1500 * 4 + 4000)
        saved = card.saved_file()
        expect(saved.exists(), f"nothing saved at {saved}")
        expect(saved.read_bytes() == AUDIO, "the saved file is not the episode, byte for byte")
        expect(not saved.with_suffix(".mp3.part").exists(), "the .part file was left behind")
        episode = card.episode(EPISODE_ID)
        expect(episode is not None and episode["saved_at"] > 0, f"not recorded as saved: {episode}")

    # The server is gone: Saved, the episode, Play.
    out = run_sim(simulator, card, (HOME_SAVED, FIRST_ROW, EPISODE_PLAY),
                  timeout_ms=2250 + 1500 * 3 + 4000, screenshot=root / "offline.ppm")
    expect_in(f"playing {EPISODE_ID} from 0 s, from the SD card", out)
    expect_in("[sim_audio] playing", out)


def test_search(simulator: Path, index: FakeIndex, root: Path) -> None:
    card = Card(root, "search", index.base)
    index.take()
    out = run_sim(simulator, card, (HOME_SEARCH,), text="test talk\n", timeout_ms=7000)
    requests = index.take()
    check_signed(requests)
    searches = [r["path"] for r in requests if r["path"].startswith("/api/1.0/search/")]
    expect(searches == ["/api/1.0/search/byterm?q=test%20talk&max=40"], f"searches: {searches}")
    expect_in("1 podcasts", out)


def test_star(simulator: Path, index: FakeIndex, root: Path) -> None:
    card = Card(root, "star", index.base)
    run_sim(simulator, card, (HOME_TRENDING, FIRST_ROW, TITLE_STAR))
    feed = card.feed(FEED_ID)
    expect(feed is not None and feed["starred_at"] > 0, f"not starred: {feed}")


def test_skip_ahead_asks_for_a_range(simulator: Path, index: FakeIndex, root: Path) -> None:
    card = Card(root, "skip", index.base)
    index.take()
    run_sim(simulator, card,
            (HOME_TRENDING, FIRST_ROW, PODCAST_FIRST_EPISODE, EPISODE_PLAY, PLAYER_AHEAD),
            timeout_ms=2250 + 1500 * 5 + 3000)
    ranges = [r["headers"].get("range", "") for r in index.take() if r["path"] == "/audio/ep1.mp3"]
    expect(len(ranges) >= 2 and ranges[0] == "", f"requests for the audio: {ranges}")
    # 30 s on from a second or two in, by the file's length over its 60 s.
    start = ID3_PADDING + 10
    offsets = [int(m.group(1)) for m in (re.match(r"bytes=(\d+)-", r) for r in ranges[1:]) if m]
    expect(offsets, f"no Range request after skipping: {ranges}")
    seconds = (offsets[-1] - start) * 60 / (len(AUDIO) - start)
    expect(30 <= seconds <= 36, f"skipped to {seconds:.1f} s, not some 30 s on")


def test_plays_on_after_leaving(simulator: Path, index: FakeIndex, root: Path) -> None:
    card = Card(root, "background", index.base)
    out = run_sim(simulator, card,
                  (HOME_TRENDING, FIRST_ROW, PODCAST_THIRD_EPISODE, EPISODE_PLAY, "home"),
                  timeout_ms=2250 + 1500 * 5 + 12000)
    expect_in(f"playing {SHORT_ID} from 0 s, streamed", out)
    left = out.find("I (podcasts) on_destroy")
    ended = out.find("played to the end")
    expect(left >= 0, "the app was not left")
    expect(ended > left, "the episode did not play on to its end after the app was left")
    episode = card.episode(SHORT_ID)
    expect(episode is not None and episode["finished"] == 1, f"not recorded as finished: {episode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sim", type=Path, default=SIM_DIR / "build" / "thistle_sim")
    parser.add_argument("--keep", action="store_true", help="keep the scratch cards and screenshots")
    args = parser.parse_args()
    simulator = args.sim.resolve()
    if not simulator.exists():
        print(f"simulator not found: {simulator}", file=sys.stderr)
        return 2

    root = Path(tempfile.mkdtemp(prefix="thistle-podcasts-"))
    failures = 0
    with FakeIndex() as index:
        cases = [
            ("browse_and_play", lambda: test_browse_and_play(simulator, index, root)),
            ("save_then_play_offline", lambda: test_save_then_play_offline(simulator, root)),
            ("search", lambda: test_search(simulator, index, root)),
            ("star", lambda: test_star(simulator, index, root)),
            ("skip_ahead_asks_for_a_range", lambda: test_skip_ahead_asks_for_a_range(simulator, index, root)),
            ("plays_on_after_leaving", lambda: test_plays_on_after_leaving(simulator, index, root)),
        ]
        for name, case in cases:
            try:
                case()
                print(f"PASS {name}")
            except AssertionError as error:
                failures += 1
                print(f"FAIL {name}: {error}")
    if args.keep:
        print(f"kept {root}")
    else:
        shutil.rmtree(root, ignore_errors=True)
    print(f"{len(cases) - failures}/{len(cases)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
