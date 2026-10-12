#!/usr/bin/env python3
"""The Assistant talks to Anthropic and OpenAI-style servers, and its setup
form saves what is typed into it.

A local server stands in for both APIs and records what the simulator sent.
Each run types a question into the Assistant on a scratch copy of
simulator/sdcard, then checks:

- the request carries its provider's own headers and body shape;
- the question is sent once, after the earlier exchanges that were answered,
  and nothing else: the Assistant used to send every question twice, and to
  pass its own error notes back to the model as things it had said;
- the reply, Unicode escapes and line breaks included, is saved with the
  conversation, and an API error is saved as a note;
- a reply that takes a long time does not hold up the UI, which once sat in
  the HTTP call for up to 30 seconds.

The setup form is then filled in from the keyboard alone, switching provider
on the way, and must write assistant.json.
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SIM_DIR = Path(__file__).resolve().parent.parent
ANTHROPIC_KEY = "sk-ant-test"
OPENAI_KEY = "sk-openai-test"
# Sent by the server as é and a surrogate pair; json.dumps escapes both.
REPLY = "Café \U0001F600\nsecond line"
OPENAI_REPLY = "Hello from a Chat Completions server"
SLOW_QUESTION = "slow"
SLOW_SECONDS = 10


class MockApi:
    """Answers as Anthropic under /anthropic and as OpenAI under /openai."""

    def __init__(self) -> None:
        self.requests: list[dict] = []
        recorder = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802 (http.server API)
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length) or b"{}")
                headers = {k.lower(): v for k, v in self.headers.items()}
                recorder.requests.append(
                    {"path": self.path, "headers": headers, "body": body})
                status, payload = recorder.answer(self.path, headers, body)
                data = json.dumps(payload).encode()
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the slow case's simulator has already exited

            def log_message(self, *args) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)

    @property
    def base(self) -> str:
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def answer(self, path: str, headers: dict, body: dict):
        messages = body.get("messages", [])
        if messages and messages[-1].get("content") == SLOW_QUESTION:
            time.sleep(SLOW_SECONDS)
        if path == "/anthropic/v1/messages":
            if headers.get("x-api-key") != ANTHROPIC_KEY:
                return 401, {"type": "error", "error": {
                    "type": "authentication_error",
                    "message": "invalid x-api-key"}}
            turn = sum(1 for m in messages if m["role"] == "user")
            return 200, {"type": "message", "role": "assistant",
                         "content": [{"type": "text",
                                      "text": f"{REPLY} #{turn}"}],
                         "stop_reason": "end_turn"}
        if path == "/openai/v1/chat/completions":
            return 200, {"object": "chat.completion", "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": OPENAI_REPLY},
                "finish_reason": "stop"}]}
        return 404, {"error": "not found"}

    def take(self) -> list[dict]:
        taken, self.requests = self.requests, []
        return taken

    def __enter__(self) -> "MockApi":
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.server.shutdown()


class Card:
    """A scratch SD card for the simulator."""

    def __init__(self, root: Path, name: str) -> None:
        self.path = root / name
        shutil.copytree(SIM_DIR / "sdcard", self.path)
        (self.path / "config" / "assistant.json").unlink(missing_ok=True)

    @property
    def config_path(self) -> Path:
        return self.path / "config" / "assistant.json"

    def write_config(self, config: dict) -> None:
        self.config_path.write_text(json.dumps(config))

    def read_config(self) -> dict:
        return json.loads(self.config_path.read_text())

    def conversation(self) -> list[tuple[str, str]]:
        path = self.path / "data" / "assistant" / "last_conversation.txt"
        turns = []
        for line in path.read_text().splitlines():
            sender, _time, text = line.split("|", 2)
            text = text.replace("\\n", "\n").replace("\\\\", "\\")
            turns.append((sender, text))
        return turns


def run_sim(simulator: Path, card: Card, device: str, text: str,
            taps: tuple[str, ...] = (), timeout_ms: int = 5000):
    command = [str(simulator), "--headless", "--device", device,
               "--timeout", str(timeout_ms),
               "--launch-app", "com.thistle.assistant", "--type", text]
    for tap in taps:
        command.extend(("--tap", tap))
    env = dict(os.environ, THISTLE_SIM_SDCARD=str(card.path))
    run = subprocess.run(command, env=env, text=True, capture_output=True,
                         check=False, timeout=60)
    if run.returncode != 0:
        raise AssertionError(
            f"simulator exited {run.returncode}: {run.stderr.strip()[-300:]!r}")
    return run


def expect(actual, expected, what: str) -> None:
    if actual != expected:
        raise AssertionError(f"{what}: expected {expected!r}, got {actual!r}")


def one_request(api: MockApi) -> dict:
    requests = api.take()
    expect(len(requests), 1, "requests sent")
    return requests[0]


def user(text: str) -> dict:
    return {"role": "user", "content": text}


def assistant(text: str) -> dict:
    return {"role": "assistant", "content": text}


def test_anthropic_conversation(simulator: Path, api: MockApi,
                                root: Path) -> None:
    card = Card(root, "anthropic")
    url = f"{api.base}/anthropic/v1/messages"
    # Written before "provider" existed: no provider, an Anthropic URL.
    card.write_config({"api_url": url, "api_key": ANTHROPIC_KEY,
                       "model": "claude-test", "system_prompt": "Be brief."})

    run_sim(simulator, card, "tdeck-max", "hello\n")
    request = one_request(api)
    headers = request["headers"]
    expect(headers.get("x-api-key"), ANTHROPIC_KEY, "x-api-key")
    expect(headers.get("anthropic-version"), "2023-06-01", "anthropic-version")
    expect(headers.get("content-type"), "application/json", "content-type")
    expect("authorization" in headers, False, "authorization header sent")
    expect(request["body"], {"model": "claude-test", "max_tokens": 512,
                             "system": "Be brief.",
                             "messages": [user("hello")]}, "first body")
    reply1 = f"{REPLY} #1"
    expect(card.conversation(), [("You", "hello"), ("AI", reply1)],
           "saved conversation")

    run_sim(simulator, card, "tdeck-max", "again\n")
    expect(one_request(api)["body"]["messages"],
           [user("hello"), assistant(reply1), user("again")],
           "second turn's messages")
    reply2 = f"{REPLY} #2"

    card.write_config({"provider": "anthropic", "api_url": url,
                       "api_key": "sk-ant-wrong", "model": "claude-test"})
    run_sim(simulator, card, "tdeck-max", "fails\n")
    api.take()
    expect(card.conversation()[-2:],
           [("You", "fails"),
            ("Note", "API error (HTTP 401): invalid x-api-key")],
           "API error note")

    card.write_config({"provider": "anthropic", "api_url": url,
                       "api_key": ANTHROPIC_KEY, "model": "claude-test"})
    run_sim(simulator, card, "tdeck-max", "last\n")
    expect(one_request(api)["body"]["messages"],
           [user("hello"), assistant(reply1), user("again"), assistant(reply2),
            user("last")],
           "messages after an error")


def test_openai_style(simulator: Path, api: MockApi, root: Path) -> None:
    card = Card(root, "openai")
    url = f"{api.base}/openai/v1/chat/completions"
    card.write_config({"provider": "openai", "api_url": url,
                       "api_key": OPENAI_KEY, "model": "gpt-test",
                       "system_prompt": "Be brief."})

    run_sim(simulator, card, "tdeck", "hi\n")
    request = one_request(api)
    expect(request["headers"].get("authorization"), f"Bearer {OPENAI_KEY}",
           "authorization")
    expect("x-api-key" in request["headers"], False, "x-api-key sent")
    expect(request["body"], {"model": "gpt-test", "messages": [
        {"role": "system", "content": "Be brief."}, user("hi")]}, "body")
    expect(card.conversation(), [("You", "hi"), ("AI", OPENAI_REPLY)],
           "saved conversation")

    # A local server takes no key, and is sent none.
    card.write_config({"provider": "openai", "api_url": url, "api_key": "",
                       "model": "local-model"})
    run_sim(simulator, card, "tdeck", "again\n")
    request = one_request(api)
    expect("authorization" in request["headers"], False,
           "authorization sent without a key")
    messages = request["body"]["messages"]
    expect(messages[0]["role"], "system", "first role (the default prompt)")
    expect(messages[1:], [user("hi"), assistant(OPENAI_REPLY), user("again")],
           "messages")


def test_slow_reply_leaves_ui_free(simulator: Path, api: MockApi,
                                   root: Path) -> None:
    card = Card(root, "slow")
    card.write_config({"provider": "openai",
                       "api_url": f"{api.base}/openai/v1/chat/completions",
                       "model": "gpt-test"})
    started = time.monotonic()
    run = run_sim(simulator, card, "tdeck-max", f"{SLOW_QUESTION}\nnext",
                  timeout_ms=4000)
    elapsed = time.monotonic() - started
    if elapsed > SLOW_SECONDS - 2:
        raise AssertionError(
            f"the simulator took {elapsed:.1f}s: the UI waited on the reply")
    # Typing went on while the request was out.
    if 'Focused text: "next"' not in run.stdout:
        raise AssertionError("keys typed during the request were lost")
    api.take()


def test_setup_form_from_keyboard(simulator: Path, root: Path) -> None:
    card = Card(root, "setup")
    gear = "221,37"  # right of the header on a 240 px screen
    # --type delivers every key at once, and the UI manager queues 32 key
    # events, so each run types at most 16 keys.

    # Focus starts on the Anthropic button. Tab to the key, type it; Enter
    # moves on to Show, Tab to the model and prompt, then Save, and Enter.
    run = run_sim(simulator, card, "tdeck-max",
                  "\t\t\tsk-form\n\t\t\t\n", taps=(gear,))
    if "Setup open" not in run.stdout:
        raise AssertionError("the gear tap did not open setup")
    if "key queue full" in run.stdout + run.stderr:
        raise AssertionError("typed faster than the key queue holds")
    config = card.read_config()
    expect(config["provider"], "anthropic", "provider")
    expect(config["api_key"], "sk-form", "api_key")
    expect(config["api_url"], "https://api.anthropic.com/v1/messages",
           "api_url")
    if not config["model"]:
        raise AssertionError("no default model saved")

    # Switch to OpenAI style: the URL follows, the model empties for one to
    # be typed, and the Anthropic key is not carried over to a new server.
    run_sim(simulator, card, "tdeck-max",
            "\t\n\t\t\t\tgpt-x\n\t\n", taps=(gear,))
    config = card.read_config()
    expect(config["provider"], "openai", "provider after switching")
    expect(config["api_url"], "https://api.openai.com/v1/chat/completions",
           "api_url after switching")
    expect(config["model"], "gpt-x", "model after switching")
    expect(config["api_key"], "", "api_key after switching")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Assistant chat and setup ===")
    failures = 0
    with MockApi() as api, tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cases = (
            ("anthropic-conversation",
             lambda: test_anthropic_conversation(simulator, api, root)),
            ("openai-style", lambda: test_openai_style(simulator, api, root)),
            ("slow-reply-leaves-ui-free",
             lambda: test_slow_reply_leaves_ui_free(simulator, api, root)),
            ("setup-form-from-keyboard",
             lambda: test_setup_form_from_keyboard(simulator, root)),
        )
        for name, case in cases:
            try:
                case()
            except (AssertionError, OSError, KeyError, ValueError,
                    subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {name}: {exc}")
            else:
                print(f"PASS  {name}")
            api.take()

    if failures:
        print(f"{failures}/{len(cases)} assistant tests failed")
        return 1
    print(f"All {len(cases)} assistant tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
