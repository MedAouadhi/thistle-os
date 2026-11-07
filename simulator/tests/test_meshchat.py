#!/usr/bin/env python3
"""The MeshCore app on the simulated T-Deck Max: the mesh conversations saved
on the card (/sdcard/messages/mesh, msg_log.rs) come back the first time it
opens after a restart, the Public channel always among them; what the node
hears lands in the Public channel, named by its writers, or in a
conversation of its own for a direct message; the conversations outlive the
app, which another app coming to the front destroys; Nodes offers Advertise
and the Public channel; and nothing is sent before the node is on air.

The simulator runs no MeshCore node (the engine is the device's), so it is
never on air there: the app says "MeshCore is starting..." over its lists,
and sending and advertising are refused, which is what is checked of them.
The simulator's --mesh-message and --mesh-direct put messages straight into
the node's inbox.

The app logs "(meshchat) history: N messages in M conversations" as it reads
the card, and the chat kit "(chatkit) meshchat: conversation TITLE: N
messages; ..." as a conversation is drawn.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"

# 2026-10-13, 06:26:40 UTC.
SAVED_AT = 1_791_880_000

# On the T-Deck Max's 240 x 320, under the status bar, the title and the
# banner: a list's first row; the Nodes tab; Home's first row of what is new.
FIRST_ROW = "120,125"
NODES_TAB = "180,300"
HOME_ROW = "100,128"

HISTORY = "(meshchat) history: "
MESHCHAT = ("--launch-app", "com.thistle.meshchat")


@dataclass(frozen=True)
class Case:
    name: str
    # Whether the card holds a mesh history.
    saved: bool
    args: tuple[str, ...]
    timeout_ms: int
    # Each a run of lines that must appear in this order.
    expected: tuple[tuple[str, ...], ...] = ()
    # Lines that must appear exactly once.
    once: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    after: Optional[Callable[[Path], Optional[str]]] = None


CASES = (
    Case("saved-conversations-come-back", True, MESHCHAT, 5000,
         # The Public channel's 2 and the contact's 1; the SMS are the SMS
         # app's.
         expected=(("(meshchat) on_create", HISTORY + "3 messages in 2 conversations",
                    "(chatkit) meshchat: page chats: 2 rows"),)),
    Case("the-public-channel-names-its-writers", True,
         MESHCHAT + ("--mesh-message", "Alice: hello mesh", "--mesh-message", "Carl: yes, two of us",
                     "--tap-start", "3500", "--tap", FIRST_ROW), 7000,
         expected=(("(meshchat) received from Alice in Public", "(meshchat) received from Carl in Public",
                    "(chatkit) meshchat: conversation Public: 4 messages"),)),
    Case("a-direct-message-starts-a-conversation", True,
         MESHCHAT + ("--mesh-direct", "Anna: psst"), 5000,
         expected=(("(meshchat) received from Anna", "(chatkit) meshchat: page chats: 3 rows"),)),
    Case("conversations-outlive-the-app", False,
         MESHCHAT + ("--mesh-direct", "Anna: psst", "--tap-start", "3500", "--tap", "home",
                     "--tap", HOME_ROW), 9000,
         # Taken from the node once, kept unread across the app's destroy:
         # Home counts it, and the app has it again when it opens.
         expected=(("(meshchat) received from Anna", "(meshchat) on_destroy",
                    "Launching app from Home: com.thistle.meshchat", "(meshchat) on_create",
                    "(chatkit) meshchat: page chats: 2 rows"),),
         once=("(meshchat) received from Anna", HISTORY)),
    Case("nothing-is-sent-before-the-node-is-on-air", True,
         MESHCHAT + ("--tap-start", "3500", "--tap", FIRST_ROW, "--type", "hi all\n"), 7000,
         expected=(("(chatkit) meshchat: conversation", "toast: MeshCore is starting..."),),
         absent=("(chatkit) meshchat: sent",)),
    Case("nodes-offers-advertise-and-the-public-channel", False,
         MESHCHAT + ("--tap-start", "3500", "--tap", NODES_TAB, "--tap", FIRST_ROW), 7000,
         # No node has advertised in the simulator; and its node is not on
         # air to advertise.
         expected=(("(chatkit) meshchat: page contacts: 2 rows", "(meshchat) advert: ",
                    "toast: Could not advertise"),)),
    Case("no-history-only-the-public-channel", False, MESHCHAT, 5000,
         expected=((HISTORY + "0 messages in 1 conversations", "(chatkit) meshchat: page chats: 1 rows"),)),
)


def write_jsonl(path: Path, lines: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(l, sort_keys=True, separators=(",", ":")) + "\n"
                            for l in lines))


def write_history(card: Path, saved: bool) -> None:
    """The card's history as msg_log.rs writes it, in place of whatever the
    simulator's card holds."""
    messages = card / "messages"
    shutil.rmtree(messages, ignore_errors=True)
    if not saved:
        return
    write_jsonl(messages / "mesh" / "public.jsonl", [
        {"dir": "in", "from": "Bob", "text": "hi all", "time": SAVED_AT - 500},
        {"dir": "out", "to": "Public", "text": "hello Bob", "time": SAVED_AT - 400},
    ])
    write_jsonl(messages / "mesh" / "36d097f0e0a2.jsonl", [
        {"dir": "in", "from": "Heltec", "text": "are you there?", "time": SAVED_AT - 300},
    ])
    write_jsonl(messages / "sms" / "+491609876543.jsonl", [
        {"dir": "in", "from": "+491609876543", "text": "Hallo", "time": SAVED_AT},
    ])


def in_order(log: str, expected: tuple[str, ...]) -> Optional[str]:
    """The first expected line missing, or out of order."""
    at = 0
    for line in expected:
        found = log.find(line, at)
        if found < 0:
            return line
        at = found + len(line)
    return None


def run_case(simulator: Path, case: Case) -> None:
    with tempfile.TemporaryDirectory(prefix="thistle-meshchat-") as tmp:
        card = Path(tmp) / "card"
        spiffs = Path(tmp) / "spiffs"
        shutil.copytree(SDCARD, card)
        spiffs.mkdir()
        write_history(card, case.saved)
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs))

        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(case.timeout_ms), *case.args]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
        log = run.stdout + run.stderr
        after = case.after(card) if case.after else None
    if run.returncode < 0:
        raise AssertionError(f"simulator died on signal {-run.returncode}")
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    for sequence in case.expected:
        missing = in_order(log, sequence)
        if missing:
            raise AssertionError(f"no {missing!r}, in order")
    for line in case.once:
        count = log.count(line)
        if count != 1:
            raise AssertionError(f"{line!r} {count} times")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"{line!r} in the log")
    if after:
        raise AssertionError(after)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== MeshCore ===")
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
        print(f"{failures}/{len(cases)} MeshCore tests failed")
        return 1
    print(f"All {len(cases)} MeshCore tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
