#!/usr/bin/env python3
"""Holding the microphone key dictates into the focused text field.

A local server stands in for Cohere's transcription API. Each run opens the
simulator on a scratch copy of simulator/sdcard whose config/dictation.json
points dictation at the server, with a key of the test's own, and plays a
WAV into the simulated microphone (THISTLE_SIM_MIC). It checks:

- a dictation into the Terminal's input arrives there, Arabic included, as
  the transcript the service gave, trimmed;
- it is sent in Arabic until the user picks English, which system.json then
  holds;
- the request was Cohere's: a POST to /v2/audio/transcriptions with the
  Bearer key, and a multipart form of the model, the language, and a 16 kHz
  mono 16-bit WAV whose samples are the microphone's from its first, none
  lost or out of order;
- with no text field focused, nothing records and nothing is sent;
- a tap of the key sends nothing;
- a refusal leaves the field as it was, and says why;
- Arabic is drawn as Arabic in Notes: in glyphs, not boxes, its letters
  joined, read from the right, and the cursor where it ends.

The real Cohere is never asked: the card's URL replaces the built-in one.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SIM_DIR = Path(__file__).resolve().parent.parent
KEY = "TESTKEY-dictation-0123456789"
MODEL = "cohere-transcribe-arabic-07-2026"
RATE = 16000
TRANSCRIPT = "  Remind me صباح الخير \n"


class FakeCohere:
    """Answers POST /v2/audio/transcriptions with `answer`, keeping each
    request for the test to look at."""

    def __init__(self) -> None:
        self.requests: list[dict] = []
        self.status = 200
        self.answer: dict = {"text": TRANSCRIPT}
        lock = threading.Lock()
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                pass

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                with lock:
                    fake.requests.append({
                        "path": self.path,
                        "headers": {k.lower(): v for k, v in self.headers.items()},
                        "body": body,
                    })
                    status, answer = fake.status, fake.answer
                # ensure_ascii, so the Arabic arrives as \u escapes.
                data = json.dumps(answer).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/v2/audio/transcriptions"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def take(self) -> list[dict]:
        taken, self.requests = self.requests, []
        return taken

    def __enter__(self) -> "FakeCohere":
        return self

    def __exit__(self, *exc) -> None:
        self.server.shutdown()
        self.server.server_close()


def make_wav(path: Path, seconds: float) -> bytes:
    """A WAV of a sweep, so that every stretch of it differs from the rest:
    a sample out of place cannot match by chance. Returns its samples."""
    frames = int(RATE * seconds)
    pcm = b"".join(
        struct.pack("<h", int(12000 * math.sin(2 * math.pi * (200 + i / 8) * i / RATE)))
        for i in range(frames))
    header = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) +
              b"data" + struct.pack("<I", len(pcm)))
    path.write_bytes(header + pcm)
    return pcm


def make_card(root: Path, name: str, url: str, language: str | None = None) -> Path:
    card = root / name
    shutil.copytree(SIM_DIR / "sdcard", card)
    (card / "config" / "dictation.json").write_text(json.dumps({"api_key": KEY, "url": url}))
    system = card / "config" / "system.json"
    settings = json.loads(system.read_text())
    settings.setdefault("thistle_os", {}).pop("dictation", None)
    if language:
        settings["thistle_os"]["dictation"] = {"language": language}
    system.write_text(json.dumps(settings, indent=2))
    return card


def run_sim(simulator: Path, card: Path, mic: Path, hold_ms: int,
            app: str | None = "com.thistle.terminal", taps: tuple[str, ...] = (),
            screenshot: Path | None = None) -> str:
    """Runs the simulator with `app` in front, makes `taps` (1.5 s apart, the
    simulator's gap), then holds the mic key for `hold_ms`."""
    command = [str(simulator), "--headless", "--device", "tdeck-max",
               "--timeout", str(2250 + 1500 * len(taps) + hold_ms + 3500)]
    for tap in taps:
        command.extend(("--tap", tap))
    command.extend(("--tap", f"mic:{hold_ms}", "--type", ""))
    if app:
        command.extend(("--launch-app", app))
    if screenshot:
        command.extend(("--screenshot", str(screenshot)))
    env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_MIC=str(mic))
    run = subprocess.run(command, env=env, text=True, capture_output=True,
                         check=False, timeout=120)
    out = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}: {out.strip()[-500:]!r}")
    return out


def expect(condition: bool, what: str) -> None:
    if not condition:
        raise AssertionError(what)


def expect_in(needle: str, out: str) -> None:
    if needle not in out:
        tail = "\n".join(out.splitlines()[-25:])
        raise AssertionError(f"{needle!r} not in the simulator's output; it ended:\n{tail}")


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """The simulator's screenshot: width, height and RGB bytes."""
    data = path.read_bytes()
    magic, width, height, depth, pixels = data.split(maxsplit=4)
    expect(magic == b"P6" and depth == b"255", f"{path} is not an 8-bit PPM")
    return int(width), int(height), pixels


def ink_columns(shot: tuple[int, int, bytes], rows: range, columns: range) -> list[int]:
    """The columns where anything dark is drawn in `rows`."""
    width, _, pixels = shot
    return [x for x in columns
            if any(pixels[(y * width + x) * 3] < 128 for y in rows)]


def form_parts(request: dict) -> dict[str, tuple[dict, bytes]]:
    """The multipart form's parts by name: their headers and content."""
    content_type = request["headers"].get("content-type", "")
    expect(content_type.startswith("multipart/form-data; boundary="),
           f"Content-Type: {content_type!r}")
    boundary = content_type.split("boundary=", 1)[1].encode()
    body = request["body"]
    expect(body.endswith(b"\r\n--" + boundary + b"--\r\n"), "the form is not closed")
    parts = {}
    for chunk in body.split(b"--" + boundary)[1:-1]:
        expect(chunk.startswith(b"\r\n") and chunk.endswith(b"\r\n"), "a part is misframed")
        head, _, content = chunk[2:-2].partition(b"\r\n\r\n")
        headers = dict(line.split(": ", 1) for line in head.decode().split("\r\n"))
        disposition = headers["Content-Disposition"]
        name = disposition.split('name="', 1)[1].split('"', 1)[0]
        parts[name] = (headers, content)
    return parts


def test_dictates_into_the_focused_field(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    mic = root / "speech.wav"
    pcm = make_wav(mic, 4.0)
    card = make_card(root, "dictate", cohere.url)
    cohere.take()
    cohere.status, cohere.answer = 200, {"text": TRANSCRIPT}
    out = run_sim(simulator, card, mic, hold_ms=1500)

    expect_in('Focused text: "Remind me صباح الخير"', out)
    expect_in("recording, in ar", out)
    expect_in(f"[sim_audio] recording 16000 Hz 1-channel from {mic}", out)
    expect_in("[sim_audio] configure sample_rate=16000 bits=16 channels=1", out)
    requests = cohere.take()
    expect(len(requests) == 1, f"{len(requests)} requests, not 1")
    request = requests[0]
    expect(request["path"] == "/v2/audio/transcriptions", f"path: {request['path']}")
    expect(request["headers"].get("authorization") == f"Bearer {KEY}",
           f"Authorization: {request['headers'].get('authorization')!r}")

    parts = form_parts(request)
    expect(list(parts) == ["model", "language", "file"], f"parts: {list(parts)}")
    expect(parts["model"][1] == MODEL.encode(), f"model: {parts['model'][1]!r}")
    expect(parts["language"][1] == b"ar", f"language: {parts['language'][1]!r}")
    headers, wav = parts["file"]
    expect(headers.get("Content-Type") == "audio/wav", f"file type: {headers}")
    expect(wav[:4] == b"RIFF" and wav[8:16] == b"WAVEfmt ", "the file is not a WAV")
    fmt = struct.unpack("<IHHIIHH", wav[16:36])
    expect(fmt == (16, 1, 1, RATE, RATE * 2, 2, 16), f"fmt: {fmt}")
    expect(wav[36:40] == b"data", "no data chunk where expected")
    size = struct.unpack("<I", wav[40:44])[0]
    sent = wav[44:]
    expect(size == len(sent) and struct.unpack("<I", wav[4:8])[0] == 36 + size,
           "the WAV's sizes disagree with what was sent")
    seconds = len(sent) / (RATE * 2)
    # Held 1.5 s, of which the codec's settling takes 0.4 s.
    expect(0.6 <= seconds <= 1.5, f"{seconds:.2f} s of audio sent")
    expect(sent == pcm[:len(sent)], "the samples sent are not the microphone's from its first")


def test_english_once_chosen(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    mic = root / "speech.wav"
    make_wav(mic, 4.0)
    card = make_card(root, "english", cohere.url, language="en")
    cohere.take()
    cohere.status, cohere.answer = 200, {"text": "Remind me to buy bread."}
    try:
        out = run_sim(simulator, card, mic, hold_ms=1500)
    finally:
        cohere.answer = {"text": TRANSCRIPT}
    expect_in("recording, in en", out)
    expect_in('Focused text: "Remind me to buy bread."', out)
    requests = cohere.take()
    expect(len(requests) == 1, f"{len(requests)} requests, not 1")
    language = form_parts(requests[0])["language"][1]
    expect(language == b"en", f"language: {language!r}")


def test_no_field_records_nothing(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    mic = root / "speech.wav"
    make_wav(mic, 4.0)
    card = make_card(root, "no-field", cohere.url)
    cohere.take()
    out = run_sim(simulator, card, mic, hold_ms=1500, app=None)
    expect_in("Pick a text field to dictate into", out)
    expect("[sim_audio] recording" not in out, "the microphone was opened")
    expect(not cohere.take(), "a request was sent")


def test_a_tap_sends_nothing(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    mic = root / "speech.wav"
    make_wav(mic, 4.0)
    card = make_card(root, "tap", cohere.url)
    cohere.take()
    out = run_sim(simulator, card, mic, hold_ms=100)
    expect_in("Hold the mic key while you speak", out)
    expect_in('Focused text: ""', out)
    expect(not cohere.take(), "a request was sent")


def test_a_refusal_changes_nothing(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    mic = root / "speech.wav"
    make_wav(mic, 4.0)
    card = make_card(root, "refused", cohere.url)
    cohere.take()
    cohere.status, cohere.answer = 401, {"id": "x", "message": "invalid api token"}
    try:
        out = run_sim(simulator, card, mic, hold_ms=1500)
    finally:
        cohere.status, cohere.answer = 200, {"text": TRANSCRIPT}
    expect_in("Cohere refused the API key", out)
    expect_in('Focused text: ""', out)
    expect(len(cohere.take()) == 1, "the recording was not sent once")


def test_arabic_is_drawn_as_arabic(simulator: Path, cohere: FakeCohere, root: Path) -> None:
    """A word, and the same letters kept apart by zero-width non-joiners, in a
    new note: the first drawn joined, so narrower; both from the right."""
    mic = root / "speech.wav"
    make_wav(mic, 4.0)
    card = make_card(root, "arabic", cohere.url)
    shot_path = root / "arabic.ppm"
    cohere.take()
    joined, apart = "سلام", "\u200c".join("سلام")
    cohere.status, cohere.answer = 200, {"text": f"{joined}\n{apart}"}
    try:
        out = run_sim(simulator, card, mic, hold_ms=1500, app="com.thistle.notes",
                      taps=("190,30",), screenshot=shot_path)
    finally:
        cohere.answer = {"text": TRANSCRIPT}
    expect_in(f'Focused text: "{joined}\n{apart}"', out)
    expect("no glyph for" not in out, "a character of the transcript has no glyph")

    cursor = [line for line in out.splitlines() if line.startswith("Focused cursor: ")]
    expect(len(cursor) == 1, "no cursor reported")
    cx, cy = (int(v) for v in cursor[0].split(": ", 1)[1].split(","))
    shot = read_ppm(shot_path)
    text_area = range(12, shot[0] - 4)
    first = ink_columns(shot, range(cy - 20, cy), text_area)
    second = ink_columns(shot, range(cy, cy + 20), text_area)
    expect(first and second, "nothing drawn on one of the note's two lines")
    first_w, second_w = first[-1] - first[0], second[-1] - second[0]
    expect(first_w * 10 < second_w * 9,
           f"joined letters drawn {first_w} px wide, apart {second_w} px: not joined")
    expect(abs(first[-1] - second[-1]) <= 1 and first[-1] > shot[0] * 3 // 4,
           f"the lines end at x {first[-1]} and {second[-1]}, not both at the right")
    expect(abs(cx - second[0]) <= 2,
           f"cursor at x {cx}, the text ends at x {second[0]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sim", type=Path, default=SIM_DIR / "build" / "thistle_sim")
    parser.add_argument("--keep", action="store_true", help="keep the scratch cards")
    args = parser.parse_args()
    simulator = args.sim.resolve()
    if not simulator.exists():
        print(f"simulator not found: {simulator}", file=sys.stderr)
        return 2

    root = Path(tempfile.mkdtemp(prefix="thistle-dictation-"))
    failures = 0
    with FakeCohere() as cohere:
        cases = [
            ("dictates_into_the_focused_field",
             lambda: test_dictates_into_the_focused_field(simulator, cohere, root)),
            ("english_once_chosen", lambda: test_english_once_chosen(simulator, cohere, root)),
            ("no_field_records_nothing", lambda: test_no_field_records_nothing(simulator, cohere, root)),
            ("a_tap_sends_nothing", lambda: test_a_tap_sends_nothing(simulator, cohere, root)),
            ("a_refusal_changes_nothing", lambda: test_a_refusal_changes_nothing(simulator, cohere, root)),
            ("arabic_is_drawn_as_arabic", lambda: test_arabic_is_drawn_as_arabic(simulator, cohere, root)),
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
