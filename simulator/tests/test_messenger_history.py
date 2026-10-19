#!/usr/bin/env python3
"""The Messenger after a restart, on the simulated T-Deck Max: the
conversations saved on the card (/sdcard/messages, msg_log.rs) come back the
first time it opens, each with its last 20 messages, and a message saved
since the restart shows once, from the kernel's inbox, not again from the
card.

The Messenger prints "(messenger_ui) history: N messages in M conversations"
as it reads the card, and "(messenger_ui) open TITLE: N messages; LVGL pool
..." as a conversation opens. A conversation of more bubbles than LVGL's
pool holds hangs the simulator, which this run's timeout catches.

Each case writes its own history to a scratch copy of simulator/sdcard:
one SMS conversation with Ben Müller (simulator/sdcard/contacts), whose
messages are the latest, and on the mesh the Public channel and a contact.
The list shows them oldest first, the SMS conversation third. Taps start
from the modem's registration, as test_home.py's do: the list is rebuilt as
the phonebook names the number, and a tap on a row being rebuilt is lost.
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
from typing import Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"

# In simulator/sdcard/contacts/contacts.vcf, as "+49 160 9876543".
NUMBER = "+491609876543"
NAME = "Ben Müller"
# 2026-10-13, 06:26:40 UTC.
SAVED_AT = 1_791_880_000

# The Messenger's list: below the 24 px status bar and its 30 px header,
# rows 48 px high. Home's row of what is new, as test_home.py taps it.
SMS_ROW = "100,174"
HOME_ROW = "100,128"
REGISTERED = "registered+2500"

HISTORY = "(messenger_ui) history: "
MESSENGER = ("--launch-app", "com.thistle.messenger")


@dataclass(frozen=True)
class Case:
    name: str
    # How many SMS the card holds for NUMBER; None for no history at all.
    saved_sms: Optional[int]
    args: tuple[str, ...]
    timeout_ms: int
    # Each a run of lines that must appear in this order.
    expected: tuple[tuple[str, ...], ...] = ()
    # Lines that must appear exactly once.
    once: tuple[str, ...] = ()


CASES = (
    Case("saved-conversations-come-back", 60, MESSENGER, 5000,
         # The SMS conversation's last 20, the Public channel's 2, the
         # contact's 1.
         expected=(("(messenger) on_create", HISTORY + "23 messages in 3 conversations"),)),
    Case("a-long-conversation-opens-with-its-last-messages", 60,
         MESSENGER + ("--tap-start", REGISTERED, "--tap", SMS_ROW), 10000,
         # Named by the phonebook, which was still being read as the
         # history came back.
         expected=((f"(messenger_ui) open {NAME}: 20 messages",),)),
    Case("an-sms-saved-since-the-restart-shows-once", 3,
         ("--sms", f"{NUMBER}: Neue Nachricht",
          "--tap-start", REGISTERED, "--tap", HOME_ROW, "--tap", SMS_ROW), 13000,
         # Saved to the card as it arrived, before the Messenger opened.
         expected=(("Launching app from Home: com.thistle.messenger",
                    HISTORY + "6 messages in 3 conversations",
                    f"(messenger_ui) open {NAME}: 4 messages"),)),
    Case("the-history-is-read-once-a-boot", 3,
         MESSENGER + ("--tap-start", REGISTERED, "--tap", "home", "--tap", f"compose:{NUMBER}"), 13000,
         expected=((HISTORY + "6 messages in 3 conversations",
                    "(messenger) on_destroy",
                    "(messenger) on_create",
                    f"(messenger_ui) compose {NUMBER}: {NAME} (existing)",
                    f"(messenger_ui) open {NAME}: 3 messages"),),
         once=(HISTORY,)),
    Case("no-history-no-conversations", None, MESSENGER, 5000,
         expected=((HISTORY + "0 messages in 0 conversations",),)),
)


def write_jsonl(path: Path, lines: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(l, sort_keys=True, separators=(",", ":")) + "\n"
                            for l in lines))


def write_history(card: Path, saved_sms: Optional[int]) -> None:
    """The card's history as msg_log.rs writes it, in place of whatever the
    simulator's card holds."""
    messages = card / "messages"
    shutil.rmtree(messages, ignore_errors=True)
    if saved_sms is None:
        return
    sms = []
    for i in range(saved_sms):
        at = SAVED_AT + i * 60
        if i % 2 == 0:
            sms.append({"dir": "in", "from": NUMBER, "text": f"Message {i}", "time": at})
        else:
            sms.append({"dir": "out", "to": NUMBER, "text": f"Reply {i}", "time": at})
    write_jsonl(messages / "sms" / f"{NUMBER}.jsonl", sms)
    write_jsonl(messages / "mesh" / "public.jsonl", [
        {"dir": "in", "from": "Bob", "text": "hi all", "time": SAVED_AT - 500},
        {"dir": "out", "to": "Public", "text": "hello Bob", "time": SAVED_AT - 400},
    ])
    write_jsonl(messages / "mesh" / "36d097f0e0a2.jsonl", [
        {"dir": "in", "from": "Heltec", "text": "are you there?", "time": SAVED_AT - 300},
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
    with tempfile.TemporaryDirectory(prefix="thistle-messenger-") as tmp:
        card = Path(tmp) / "card"
        spiffs = Path(tmp) / "spiffs"
        shutil.copytree(SDCARD, card)
        spiffs.mkdir()
        write_history(card, case.saved_sms)
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card), THISTLE_SIM_SPIFFS=str(spiffs))

        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--timeout", str(case.timeout_ms), *case.args]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
    log = run.stdout + run.stderr
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== Messenger history ===")
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
        print(f"{failures}/{len(cases)} Messenger history tests failed")
        return 1
    print(f"All {len(cases)} Messenger history tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
