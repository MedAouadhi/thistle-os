#!/usr/bin/env python3
"""Voice calls on the simulated T-Deck Max: the call screen opens for a call
ringing here or placed, its buttons answer, decline, end, mute, switch the
speaker and send DTMF, Back tucks a call away to the status bar and a tap
there brings it back, and the Messenger opens the conversation another app
asks it for.

The simulated modem (drivers/sim_cellular.c) rings with --call 3 s after it
registers, which wanders with the boot by seconds, so the taps of a case
with an incoming call start from the ring (--tap-start ring+MS), and those of
one that dials from the registration (registered+MS). A dial is a step in
place of a tap (--tap dial:NUMBER) rather than the Phone app's keypad, and a
compose request one too (--tap compose:NUMBER[:NAME]), as the Phone app's
Message button makes it. The simulator runs in real time and the kernel asks
the modem every 500 ms, so the timeouts are generous.

Coordinates are the call screen's own layout on the T-Deck Max (240 x 320,
the status bar 24 px): it fills the screen below the bar, the name and number
at the top and the buttons at the bottom (components/ui/src/call_ui.c).

The simulator's SD card is simulator/sdcard, which is in the repository. Each
case runs with its phone/ folder (the call log, the stars) moved aside, so a
missed call counts from none, and puts it and system.json back afterwards.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"
SYSTEM_JSON = SDCARD / "config" / "system.json"
PHONE_DIR = SDCARD / "phone"

CALLER = "+4915123456789"
DIALLED = "+4930123456"
BUSY = "+4930120000"

# Ringing: Silence above Decline and Answer.
SILENCE = "120,228"
DECLINE = "61,281"
ANSWER = "178,281"
# Placing or connected: Mute, Keypad, Speaker; volume - and +; End call.
MUTE = "42,207"
KEYPAD = "120,207"
SPEAKER = "197,207"
END_CALL = "120,293"
# The keypad: 3 x 4 keys, then Hide and End.
KEY_5 = "120,179"
# The status bar's call chip, "<1m" a moment after a call is tucked away,
# left of the battery; the network reading makes room for it on 240 px.
CHIP = "148,12"

RING = "ring+2500"
REGISTERED = "registered+2500"


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    timeout_ms: int
    # Each a run of lines that must appear in this order. The UI, the
    # kernel's call thread and the simulator's main loop print from threads
    # of their own, so a run keeps to one of them, or to lines a poll apart.
    expected: tuple[tuple[str, ...], ...] = ()
    absent: tuple[str, ...] = ()
    device: str = "tdeck-max"


CASES = (
    Case("incoming-call-rings-and-opens-the-screen",
         ("--call", CALLER), 16000,
         expected=((f"[sim_cellular] call ringing from {CALLER}",
                    f"[call] incoming call from {CALLER}"),
                   (f"[sim_cellular] call ringing from {CALLER}",
                    "(call_ui) open incoming"))),
    Case("answer-and-end-by-tap",
         ("--call", CALLER, "--tap-start", RING,
          "--tap", ANSWER, "--tap", "wait", "--tap", END_CALL), 28000,
         expected=(("(call_ui) open incoming", "(call_ui) answered", "(call_ui) end call",
                    "(call_ui) ended shown: Call ended", "(call_ui) closed"),
                   ("[sim_cellular] answered", "[sim_audio] route voice, speaker on",
                    "[sim_cellular] hung up", "[sim_audio] route codec"))),
    Case("unanswered-call-is-missed",
         ("--call", CALLER), 42000,
         expected=(("(call_ui) open incoming", "(call_ui) ended shown: Missed call",
                    "(call_ui) closed"),
                   ("(call_ui) ended shown: Missed call", "(call_ui) missed calls: 1"),
                   ("[sim_cellular] call ringing", "[sim_cellular] caller gave up")),
         absent=("[sim_audio] route voice",)),
    Case("decline-by-tap",
         ("--call", CALLER, "--tap-start", RING, "--tap", DECLINE), 24000,
         expected=(("(call_ui) open incoming", "(call_ui) declined",
                    "(call_ui) ended shown: Declined", "(call_ui) closed"),
                   ("[sim_cellular] call ringing", "[sim_cellular] declined"),
                   ("ended: declined",)),
         absent=("[sim_audio] route voice",)),
    Case("back-silences-a-ringing-call-and-home-leaves-it-in-front",
         ("--launch-app", "com.thistle.todo", "--call", CALLER, "--tap-start", RING,
          "--tap", "back", "--tap", "home", "--tap", SILENCE), 22000,
         expected=(("(todo) on_create", "(call_ui) open incoming", "(call_ui) silenced"),),
         absent=("(call_ui) tucked away", "(todo) on_destroy", "(call_ui) closed")),
    Case("no-quick-panel-over-the-call-screen",
         ("--call", CALLER, "--tap-start", RING,
          "--drag", "120,8,120,160", "--tap", "quick"), 22000,
         expected=(("(call_ui) open incoming",),),
         absent=("(quick_panel) open",)),
    Case("outgoing-call-dials-rings-and-connects",
         ("--tap-start", REGISTERED, "--tap", f"dial:{DIALLED}"), 22000,
         expected=((f"Dial {DIALLED}: ESP_OK",),
                   (f"[sim_cellular] dialing {DIALLED}", "[sim_cellular] alerting",
                    "[sim_cellular] active", "[sim_audio] route voice, speaker on"),
                   ("(call_ui) open outgoing",))),
    Case("busy-number-fails",
         ("--tap-start", REGISTERED, "--tap", f"dial:{BUSY}"), 22000,
         expected=((f"[sim_cellular] dialing {BUSY}", "[sim_cellular] busy"),
                   ("(call_ui) open outgoing", "(call_ui) ended shown: Call failed",
                    "(call_ui) closed")),
         absent=("[sim_cellular] active",)),
    Case("far-end-hangs-up",
         ("--call-hangup", "2", "--tap-start", REGISTERED, "--tap", f"dial:{DIALLED}"), 24000,
         expected=(("[sim_cellular] active", "[sim_audio] route voice, speaker on",
                    "[sim_cellular] far end hung up", "[sim_audio] route codec"),
                   ("(call_ui) open outgoing", "(call_ui) ended shown: Call ended",
                    "(call_ui) closed"))),
    Case("back-tucks-a-call-away-and-the-status-bar-brings-it-back",
         ("--call", CALLER, "--tap-start", RING,
          "--tap", ANSWER, "--tap", "wait", "--tap", "back", "--tap", CHIP,
          "--tap", END_CALL), 32000,
         expected=(("(call_ui) answered", "(call_ui) tucked away", "(call_ui) reopened",
                    "(call_ui) end call", "(call_ui) ended shown: Call ended"),)),
    Case("esc-tucks-a-call-away-and-stays-in-the-app",
         ("--launch-app", "com.thistle.todo", "--call", CALLER, "--tap-start", RING,
          "--tap", ANSWER, "--tap", "wait", "--type", "\x1b"), 26000,
         expected=(("(todo) on_create", "(call_ui) answered", "(call_ui) tucked away"),),
         absent=("(todo) on_destroy",)),
    Case("mute-speaker-and-dtmf",
         ("--call", CALLER, "--tap-start", RING,
          "--tap", ANSWER, "--tap", "wait", "--tap", MUTE, "--tap", SPEAKER,
          "--tap", KEYPAD, "--tap", KEY_5, "--type", "9#"), 34000,
         expected=(("(call_ui) answered", "(call_ui) mute on", "(call_ui) speaker off",
                    "(call_ui) dtmf 5", "(call_ui) dtmf 9", "(call_ui) dtmf #"),
                   ("[sim_cellular] mute on", "[sim_cellular] speaker off",
                    "[sim_cellular] dtmf 5", "[sim_cellular] dtmf 9",
                    "[sim_cellular] dtmf #"),
                   ("[sim_audio] route voice, speaker on",
                    "[sim_audio] route voice, speaker off"))),
    Case("messenger-opens-the-conversation-asked-for",
         ("--tap", f"compose:{CALLER}:Sam Taylor"), 8000,
         expected=((f"Compose to {CALLER}", "(messenger) on_create",
                    f"(messenger_ui) compose {CALLER}: Sam Taylor"),)),
    Case("messenger-in-front-takes-the-request-too",
         ("--launch-app", "com.thistle.messenger", "--tap", f"compose:{CALLER}"), 9000,
         expected=(("(messenger) on_create", f"Compose to {CALLER}",
                    f"(messenger_ui) compose {CALLER}"),),
         absent=("(messenger) on_destroy",)),
)

# The call screen's layout, for a made-up call, on the e-paper Max and on the
# LCD T-Deck's 320 x 240, which has no modem.
for _device in ("tdeck-max", "tdeck"):
    for _what in ("incoming", "calling", "active", "keypad", "ended"):
        CASES += (Case(f"layout-{_what}-fits-{_device}",
                       ("--call-preview", _what, "--report-offscreen"), 4500,
                       expected=((f"(call_ui) preview {_what}", "(call_ui) open",
                                  "Off-screen objects: 0"),),
                       device=_device),)


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
    command = [str(simulator), "--headless", "--device", case.device,
               "--timeout", str(case.timeout_ms), *case.args]
    run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                         check=False, timeout=case.timeout_ms / 1000 + 30)
    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    offscreen = [line for line in run.stdout.splitlines() if line.startswith("Off screen: ")]
    if offscreen:
        raise AssertionError("wider than the screen: " + "; ".join(offscreen))
    for sequence in case.expected:
        missing = in_order(log, sequence)
        if missing:
            raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")


def stash_phone_dir() -> Optional[Path]:
    """Move phone/ aside, so a case starts with no call log."""
    if not PHONE_DIR.exists():
        return None
    stash = Path(tempfile.mkdtemp(prefix="thistle-phone-")) / "phone"
    shutil.move(str(PHONE_DIR), str(stash))
    return stash


def restore_phone_dir(stash: Optional[Path]) -> None:
    if PHONE_DIR.exists():
        shutil.rmtree(PHONE_DIR)
    if stash is not None:
        shutil.move(str(stash), str(PHONE_DIR))
        shutil.rmtree(stash.parent, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== Calls ===")
    original = SYSTEM_JSON.read_bytes()
    failures = 0
    try:
        for case in cases:
            stash = stash_phone_dir()
            try:
                run_case(simulator, case)
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.name}: {exc}")
            else:
                print(f"PASS  {case.name}")
            finally:
                restore_phone_dir(stash)
                SYSTEM_JSON.write_bytes(original)
    finally:
        SYSTEM_JSON.write_bytes(original)

    if failures:
        print(f"{failures}/{len(cases)} call tests failed")
        return 1
    print(f"All {len(cases)} call tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
