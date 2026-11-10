#!/usr/bin/env python3
"""The SMS app on the simulated T-Deck Max: the conversations saved on the
card (/sdcard/messages/sms, msg_log.rs) come back the first time it opens
after a restart, each with its last 20 messages, and a message saved since
shows once, from the kernel's inbox, not again from the card; an SMS that
arrives opens and is answered, the answer sent on the app's own task;
another app's request for a conversation (the Phone app's Message) opens
the one already there; Contacts searches the phonebook and offers a number
typed whole.

The app logs "(sms) history: N messages in M conversations" as it reads the
card, and the chat kit "(chatkit) sms: conversation TITLE: N messages; LVGL
pool ..." as a conversation is drawn. A conversation of more bubbles than
LVGL's pool holds would hang the simulator, which the run's timeout catches.

Each case writes its own history to a scratch copy of simulator/sdcard: one
conversation with Ben Müller (simulator/sdcard/contacts), and on the mesh,
which the SMS app must leave alone, the Public channel and a contact. Taps
start from the modem's registration: the list is rebuilt as the phonebook
names the number, and a tap on a row being rebuilt is lost.
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

# In simulator/sdcard/contacts/contacts.vcf, as "+49 160 9876543".
NUMBER = "+491609876543"
NAME = "Ben Müller"
STRANGER = "+491601112223"
# 2026-10-13, 06:26:40 UTC.
SAVED_AT = 1_791_880_000

# On the T-Deck Max's 240 x 320: the first row of a list, under the status
# bar and the title, and the Contacts tab.
FIRST_ROW = "120,100"
CONTACTS_TAB = "180,300"
REGISTERED = "registered+2500"

HISTORY = "(sms) history: "
SMS = ("--launch-app", "com.thistle.sms")


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
    absent: tuple[str, ...] = ()
    # Checks the card after the run.
    after: Optional[Callable[[Path], Optional[str]]] = None


def answer_saved(card: Path) -> Optional[str]:
    """The answer is in the conversation's file, as sent."""
    path = card / "messages" / "sms" / f"{STRANGER}.jsonl"
    if not path.is_file():
        return f"no {path.name}"
    lines = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
    if not any(l.get("dir") == "out" and l.get("text") == "Yes!" for l in lines):
        return f"no answer in {path.name}: {lines}"
    return None


CASES = (
    Case("saved-conversations-come-back", 60, SMS, 5000,
         # The SMS conversation's last 20; the mesh's are MeshCore's.
         expected=(("(sms) on_create", HISTORY + "20 messages in 1 conversations"),)),
    Case("a-long-conversation-opens-with-its-last-messages", 60,
         SMS + ("--tap-start", REGISTERED, "--tap", FIRST_ROW), 10000,
         # Named by the phonebook, which was still being read as the
         # history came back.
         expected=((f"(chatkit) sms: conversation {NAME}: 20 messages",),)),
    Case("an-sms-saved-since-the-restart-shows-once", 3,
         SMS + ("--sms", f"{NUMBER}: Neue Nachricht", "--tap-start", REGISTERED, "--tap", FIRST_ROW), 13000,
         expected=((HISTORY + "3 messages in 1 conversations",
                    "(sms) received from",
                    f"(chatkit) sms: conversation {NAME}: 4 messages"),)),
    Case("an-sms-is-answered", None,
         SMS + ("--sms", f"{STRANGER}: Are you coming tonight?", "--tap-start", REGISTERED,
                "--tap", FIRST_ROW, "--type", "Yes!\n"), 13000,
         expected=(("(sms) received from " + STRANGER,
                    f"(chatkit) sms: conversation {STRANGER}: 1 messages",
                    "(chatkit) sms: sent, waiting for word",
                    "(sms) sent 1: ESP_OK",
                    f"(chatkit) sms: conversation {STRANGER}: 2 messages"),),
         after=answer_saved),
    Case("the-history-is-read-once-a-boot", 3,
         SMS + ("--tap-start", REGISTERED, "--tap", "home", "--tap", f"compose:{NUMBER}"), 13000,
         expected=((HISTORY + "3 messages in 1 conversations",
                    "(sms) on_destroy",
                    "(sms) on_create",
                    f"(sms) compose {NUMBER}: {NAME} (existing)",
                    f"(chatkit) sms: conversation {NAME}: 3 messages"),),
         once=(HISTORY,)),
    Case("a-request-matches-the-number-however-it-is-written", 3,
         ("--tap-start", REGISTERED, "--tap", "compose:+49 160 9876543"), 10000,
         # Named by the number until the phonebook, read for the first time
         # as the app opened, names it.
         expected=((f"(sms) compose {NUMBER}: ", f"{NUMBER} (existing)",
                    f"(chatkit) sms: conversation {NAME}: 3 messages"),)),
    Case("contacts-searches-the-phonebook", None,
         SMS + ("--tap-start", "3000", "--tap", CONTACTS_TAB, "--type", "ben"), 8000,
         # Ben Müller's two numbers and Karim Ben Salah's one.
         expected=(("(chatkit) sms: page contacts: 3 rows",),)),
    Case("a-number-typed-whole-is-texted", None,
         SMS + ("--tap-start", "3000", "--tap", CONTACTS_TAB, "--type", "0170 5550123\n"), 9000,
         expected=(("(sms) compose 01705550123: 01705550123",
                    "(chatkit) sms: conversation 01705550123: 0 messages"),)),
    Case("no-history-no-conversations", None, SMS, 5000,
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
    with tempfile.TemporaryDirectory(prefix="thistle-sms-") as tmp:
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
    print("=== SMS ===")
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
        print(f"{failures}/{len(cases)} SMS tests failed")
        return 1
    print(f"All {len(cases)} SMS tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
