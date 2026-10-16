#!/usr/bin/env python3
"""The Hermes app reaches a Hermes Device Bridge over the tailnet and talks
to it as hermes_esp32's device does.

A fake bridge (fake_hermes_bridge.py) speaks protocol version 1 as the
plugin does and records every frame the simulator sends. Each case runs the
app on a scratch copy of simulator/sdcard, with the simulator's pretend
tailnet joined and on unless the case says otherwise, and the bridge's
address in the card's config/hermes.json. It checks that:

- the session comes up: the WebSocket upgrade, HELLO with this device's
  capabilities, the sync, the bridge's pings answered, every frame masked;
- a typed question is sent as CHAT_SUBMIT, and the reply, edited as it
  streams, ends up drawn after the tool's activity row;
- Stop, New conversation, a switch to another Hermes Session, Scheduled
  Runs and the Command panel send what they should;
- a refusal and an incompatible bridge are said and not hammered, and a lost
  session is opened again;
- nothing is attempted while Tailscale is off, and with no address the app
  asks for one and keeps what it is given.

Coordinates are the T-Deck Max's (240 x 320). Taps start 2.25 s after
startup and come 1.5 s apart; typing comes after the last tap, and the
simulator takes at most 16 keys at once.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fake_hermes_bridge import OTHER_CONVERSATION_SESSION, FakeBridge  # noqa: E402

SIM_DIR = Path(__file__).resolve().parent.parent
APP = "com.thistle.hermes"
MENU = "220,37"
COMMANDS = "190,37"
SEND = "222,300"
NEW = "40,108"
SCHEDULED = "128,108"
SECOND_SESSION = "120,245"
FIRST_ROW = "120,120"
OLDER = "42,200"
CAPABILITIES = ["bridge_session", "device_chat", "command_descriptors", "structured_progress",
                "chat_history", "session_list", "scheduled_runs"]


@dataclass
class Run:
    log: str
    bridge: FakeBridge
    card: Path


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    timeout_ms: int
    mode: str = "normal"
    tailnet: bool = True
    address: bool = True
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    check: Optional[Callable[[Run], Optional[str]]] = field(default=None)


def in_order(log: str, expected: tuple[str, ...]) -> Optional[str]:
    at = 0
    for line in expected:
        found = log.find(line, at)
        if found < 0:
            return line
        at = found + len(line)
    return None


def sent(run: Run, kind: str) -> list[dict]:
    return [m for m in run.bridge.received if m.get("type") == kind]


def session_was_sound(run: Run) -> Optional[str]:
    hello = sent(run, "HELLO")
    if not hello:
        return "no HELLO"
    h = hello[0]
    if (h.get("min_version"), h.get("max_version")) != (1, 1):
        return f"versions {h.get('min_version')}-{h.get('max_version')}"
    if h.get("implementation", {}).get("name") != "thistle-os":
        return f"implementation {h.get('implementation')}"
    if h.get("capabilities") != CAPABILITIES:
        return f"capabilities {h.get('capabilities')}"
    if run.bridge.request_lines[:1] != ["GET / HTTP/1.1"]:
        return f"upgrade request {run.bridge.request_lines[:1]}"
    if run.bridge.unmasked:
        return f"{run.bridge.unmasked} frames were not masked"
    # Each connection's last ping may cross its close.
    if run.bridge.pings and run.bridge.pongs < run.bridge.pings - run.bridge.connections:
        return f"{run.bridge.pongs} pongs for {run.bridge.pings} pings"
    return None


def answered(run: Run) -> Optional[str]:
    problem = session_was_sound(run)
    if problem:
        return problem
    submits = sent(run, "CHAT_SUBMIT")
    if [m.get("text") for m in submits] != ["Prices?"]:
        return f"submitted {[m.get('text') for m in submits]}"
    if not re.fullmatch(r"[0-9a-f]{16}", submits[0].get("request_id", "")):
        return f"request_id {submits[0].get('request_id')!r}"
    drawn = re.findall(r"transcript: (\d+) items", run.log)
    # The two from the history, the question, the tool's row, the reply.
    if not drawn or drawn[-1] != "5":
        return f"the transcript ended with {drawn[-1:] or 'nothing'} items, not 5"
    if 'Focused text: ""' not in run.log:
        return "the draft was not cleared"
    return None


def stopped(run: Run) -> Optional[str]:
    controls = [m.get("action") for m in sent(run, "CHAT_CONTROL")]
    return None if controls == ["stop"] else f"controls {controls}"


def started_afresh(run: Run) -> Optional[str]:
    controls = [m.get("action") for m in sent(run, "CHAT_CONTROL")]
    if controls != ["new"]:
        return f"controls {controls}"
    drawn = re.findall(r"transcript: (\d+) items", run.log)
    return None if drawn and drawn[-1] == "1" else f"after the reset {drawn[-1:]} items, not the hint"


def switched(run: Run) -> Optional[str]:
    selects = sent(run, "SESSION_SELECT")
    if len(sent(run, "SESSION_LIST_REQUEST")) != 1:
        return "the drawer did not ask for the sessions once"
    if [m.get("session_id") for m in selects] != [OTHER_CONVERSATION_SESSION]:
        return f"selected {[m.get('session_id') for m in selects]}"
    return None


def read_runs(run: Run) -> Optional[str]:
    asks = [(m.get("job_id"), m.get("offset")) for m in sent(run, "SCHEDULED_RUN_REQUEST")]
    if len(sent(run, "SCHEDULED_JOBS_REQUEST")) != 1:
        return "the jobs were not asked for once"
    return None if asks == [("b7c2d4e91a05", 0), ("b7c2d4e91a05", 1)] else f"runs asked {asks}"


def wrote_command(run: Run) -> Optional[str]:
    if 'Focused text: "/new x"' not in run.log:
        found = re.findall(r'Focused text: "(.*)"', run.log)
        return f"the draft is {found}"
    if sent(run, "CHAT_SUBMIT") or sent(run, "CHAT_CONTROL"):
        return "choosing a command sent something"
    return None


def one_connection(run: Run) -> Optional[str]:
    return None if run.bridge.connections == 1 else f"{run.bridge.connections} connections"


def reconnected(run: Run) -> Optional[str]:
    if run.bridge.connections < 2:
        return f"{run.bridge.connections} connection(s)"
    # The second sync sends the same messages: on e-paper, nothing to redraw.
    drawn = re.findall(r"transcript: \d+ items", run.log)
    if len(drawn) != 1:
        return f"the transcript was drawn {len(drawn)} times, not once"
    return session_was_sound(run)


def never_connected(run: Run) -> Optional[str]:
    return None if run.bridge.connections == 0 else f"{run.bridge.connections} connections"


def kept_the_address(run: Run) -> Optional[str]:
    path = run.card / "config" / "hermes.json"
    if not path.exists():
        return "config/hermes.json was not written"
    saved = json.loads(path.read_text()).get("bridge_url")
    if saved != run.bridge.url:
        return f"saved {saved!r}"
    return one_connection(run)


CASES = (
    Case("syncs-and-answers", ("--tap", "wait", "--tap", "wait", "--type", "Prices?\n"), 9500,
         expected=("(hermes) connecting to ws://127.0.0.1:", "(hermes) connected; HELLO sent",
                   "(hermes) session up as thistle-sim: device_chat chat_history session_list "
                   "scheduled_runs",
                   "(hermes) synced: 2 rows, 14 commands", "(hermes_ui) chat shown",
                   "(hermes) sent CHAT_SUBMIT"),
         absent=("cannot read", "could not send"),
         check=answered),
    Case("stop-stops-a-turn", ("--tap", "wait", "--tap", SEND), 5500, mode="busy",
         expected=("(hermes) sent CHAT_CONTROL",), check=stopped),
    Case("new-conversation", ("--tap", "wait", "--tap", MENU, "--tap", NEW), 7000,
         expected=("(hermes_ui) panel: Hermes", "(hermes) sent CHAT_CONTROL"),
         check=started_afresh),
    Case("switches-to-another-session",
         ("--tap", "wait", "--tap", MENU, "--tap", SECOND_SESSION), 7000,
         expected=("(hermes) sent SESSION_LIST_REQUEST",
                   f"(hermes_ui) switching to session {OTHER_CONVERSATION_SESSION}",
                   "(hermes) sent SESSION_SELECT", "(hermes) synced: 2 rows"),
         check=switched),
    Case("reads-scheduled-runs",
         ("--tap", "wait", "--tap", MENU, "--tap", SCHEDULED, "--tap", FIRST_ROW, "--tap", OLDER),
         10000,
         expected=("(hermes_ui) panel: Scheduled Runs", "(hermes_ui) panel: Blog monitor"),
         check=read_runs),
    Case("command-panel-writes-the-draft",
         ("--tap", "wait", "--tap", COMMANDS, "--tap", FIRST_ROW, "--type", "x"), 7000,
         expected=("(hermes_ui) panel: Commands",), check=wrote_command),
    Case("a-refusal-is-said-and-not-hammered", (), 6000, mode="refuse",
         expected=("(hermes) refused: This tailnet node is not an authorized Hermes device.",),
         absent=("chat shown",), check=one_connection),
    Case("an-incompatible-bridge-is-said", (), 6000, mode="incompatible",
         expected=("(hermes) incompatible; update the plugin",),
         absent=("chat shown",), check=one_connection),
    Case("a-lost-session-is-opened-again", (), 7000, mode="drop",
         expected=("(hermes) synced: 2 rows", "(hermes) The bridge closed the session (1011).",
                   "(hermes) trying again in 1 s", "(hermes) synced: 2 rows"),
         check=reconnected),
    Case("waits-for-tailscale", (), 4500, tailnet=False,
         expected=("(hermes_ui) created; bridge ws://127.0.0.1:",),
         absent=("(hermes) connecting",), check=never_connected),
)

# Only where the build has no address of its own from .env.
ASK_CASE = Case("asks-for-an-address-and-keeps-it", ("--tap", "wait"), 7000, address=False,
                expected=("(hermes_ui) created; bridge not set", "(hermes_ui) panel: Bridge address",
                          "(hermes_ui) bridge address ws://127.0.0.1:", "(hermes) synced: 2 rows"),
                check=kept_the_address)


def built_in_address(simulator: Path) -> bool:
    header = simulator.parent / "hermes" / "hermes_url_default.h"
    return header.exists() and "HERMES_URL_DEFAULT" in header.read_text()


def run_case(simulator: Path, case: Case, root: Path) -> None:
    card = root / case.name
    shutil.copytree(SIM_DIR / "sdcard", card)
    spiffs = root / f"{case.name}-spiffs"
    spiffs.mkdir()
    (spiffs / ".tailnet_joined").write_text("1")
    system_path = card / "config" / "system.json"
    system = json.loads(system_path.read_text())
    system.get("thistle_os", system)["tailnet"] = {"enabled": case.tailnet}
    system_path.write_text(json.dumps(system, indent=2))
    (card / "config" / "hermes.json").unlink(missing_ok=True)

    bridge = FakeBridge(case.mode).start()
    try:
        args = list(case.args)
        if case.address:
            (card / "config" / "hermes.json").write_text(json.dumps({"bridge_url": bridge.url}))
        else:
            # Typed on the page that opens by itself: host and port, Enter.
            args += ["--type", f"127.0.0.1:{bridge.port}\n"]
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs))
        for name in ("THISTLE_SIM_TAILNET", "THISTLE_SIM_TAILNET_PEERS", "THISTLE_SIM_TAILNET_LOGIN_S"):
            env.pop(name, None)
        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(case.timeout_ms), "--launch-app", APP, *args]
        result = subprocess.run(command, env=env, text=True, capture_output=True,
                                check=False, timeout=90)
    finally:
        bridge.stop()
    log = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(f"simulator exited {result.returncode}: {log.strip()[-300:]!r}")
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    if case.check:
        problem = case.check(Run(log, bridge, card))
        if problem:
            raise AssertionError(problem)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run the case of this name alone")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = list(CASES)
    if built_in_address(simulator):
        print(f"SKIP  {ASK_CASE.name}: this build has an address from .env")
    else:
        cases.append(ASK_CASE)
    cases = [c for c in cases if not args.only or c.name == args.only]

    print("=== Hermes ===")
    failures = 0
    with tempfile.TemporaryDirectory(prefix="thistle-hermes-") as tmp:
        for case in cases:
            try:
                run_case(simulator, case, Path(tmp))
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.name}: {exc}")
            else:
                print(f"PASS  {case.name}")

    if failures:
        print(f"{failures}/{len(cases)} Hermes tests failed")
        return 1
    print(f"All {len(cases)} Hermes tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
