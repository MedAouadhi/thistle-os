#!/usr/bin/env python3
"""The Phone app lists the contacts on the SD card, filters them as the
keyboard types, stars them into Favourites, hands a number to the Messenger,
dials from its keypad, and fits every screen it opens on.

The contacts are simulator/sdcard/contacts/contacts.vcf: fifteen cards in the
formats phones export, sorted by the kernel as Anna Schmidt first.

Coordinates are the app's own layout on the T-Deck Max (240 x 320, a 24 px
status bar): the tab bar is the bottom 40 px, its tabs 60 px wide; the
contacts list starts under the search field, 31 px a row; a contact's page
has its star at the right of a 30 px header and its first number's Call and
Message side by side under the name. On the T-Deck (320 x 240) the tabs are
80 px wide at y 220.

What the kernel writes for the app, the stars and the call log, is under
simulator/sdcard/phone, which is git-ignored; every case puts it back as it
found it.

Calls themselves, and the Messenger taking the number, are other parts'
work: these cases check only that the app asked for them.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

REPO = Path(__file__).resolve().parents[2]
PHONE_DIR = REPO / "simulator" / "sdcard" / "phone"
STARRED = PHONE_DIR / "starred.json"

# T-Deck Max
TAB_FAVOURITES = "30,300"
TAB_RECENTS = "90,300"
TAB_CONTACTS = "150,300"
TAB_KEYPAD = "210,300"
FIRST_CONTACT = "120,75"
STAR = "218,39"
CALL = "62,207"
MESSAGE = "178,207"
KEY = {"1": "42,102", "4": "42,140", "5": "119,140", "9": "196,178", "0": "119,216"}
KEYPAD_CALL = "81,254"

# T-Deck
LCD_TABS = {"favourites": "40,220", "recents": "120,220", "contacts": "200,220", "keypad": "280,220"}
LCD_FIRST_CONTACT = "160,76"


@dataclass(frozen=True)
class Case:
    name: str
    device: str
    args: tuple[str, ...]
    steps: int
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    fits: bool = False
    after: Optional[Callable[[], Optional[str]]] = field(default=None)


def starred_anna() -> Optional[str]:
    """Why the star did not reach the card, or None."""
    if not STARRED.is_file():
        return f"{STARRED} was not written"
    if "Anna Schmidt" not in STARRED.read_text(encoding="utf-8"):
        return f"{STARRED} does not name Anna Schmidt"
    return None


def taps(*points: str) -> tuple[str, ...]:
    out: list[str] = []
    for point in points:
        out += ["--tap", point]
    return tuple(out)


CASES = (
    Case("opens-on-contacts-filled-from-the-card", "tdeck-max", (), 0,
         expected=("(phone) on_create", "(phone) tab contacts",
                   "(phone) contacts: 15 shown of 15")),
    Case("typing-filters-by-name-ignoring-accents", "tdeck-max", ("--type", "jur"), 0,
         expected=("(phone) contacts: 15 shown of 15", "(phone) contacts: 1 shown of 1",
                   'Focused text: "jur"')),
    Case("typing-digits-filters-by-number", "tdeck-max", ("--type", "151"), 0,
         expected=("(phone) contacts: 15 shown of 15", "(phone) contacts: 1 shown of 1",
                   'Focused text: "151"')),
    Case("enter-opens-the-contact-found", "tdeck-max", ("--type", "jur\n"), 0,
         expected=("(phone) contacts: 1 shown of 1", "(phone) page Jürgen Weber")),
    Case("a-tap-opens-a-contact-and-back-returns", "tdeck-max",
         taps(FIRST_CONTACT, "back"), 2,
         expected=("(phone) page Anna Schmidt", "(phone) tab contacts"),
         absent=("(phone) on_destroy",)),
    Case("a-star-makes-a-favourite", "tdeck-max",
         taps(FIRST_CONTACT, STAR, TAB_FAVOURITES), 3,
         expected=("(phone) page Anna Schmidt", "(phone) star Anna Schmidt on",
                   "(phone) tab favourites", "(phone) favourites: 1"),
         after=starred_anna),
    Case("message-opens-the-messenger", "tdeck-max",
         taps(FIRST_CONTACT, MESSAGE), 2,
         expected=("(phone) page Anna Schmidt", "(phone) message +4915123456789",
                   "(messenger) on_create")),
    Case("call-from-a-contact", "tdeck-max",
         taps(FIRST_CONTACT, CALL), 2,
         expected=("(phone) page Anna Schmidt", "(phone) call +49 151 23456789")),
    Case("recents-empty", "tdeck-max", taps(TAB_RECENTS), 1,
         expected=("(phone) tab recents", "(phone) recents: 0"), fits=True),
    Case("keypad-enter-calls-what-was-typed", "tdeck-max",
         taps(TAB_KEYPAD) + ("--type", "+4915123456789\n"), 1,
         expected=("(phone) tab keypad", "(phone) call +4915123456789")),
    # Held taps: a long press on 0 types "+", every other key types itself.
    Case("keypad-keys-and-call-button", "tdeck-max",
         ("--tap-hold", "700") + taps(TAB_KEYPAD, KEY["0"], KEY["4"], KEY["9"], KEY["1"],
                                      KEY["5"], KEY["1"], KEYPAD_CALL), 8,
         expected=("(phone) tab keypad", "(phone) call +49151")),
    Case("eink-keypad-fits", "tdeck-max", taps(TAB_KEYPAD), 1,
         expected=("(phone) tab keypad",), fits=True),
    Case("eink-contact-page-fits", "tdeck-max", taps(FIRST_CONTACT), 1,
         expected=("(phone) page Anna Schmidt",), fits=True),
) + tuple(
    Case(f"lcd-{tab}-fits", "tdeck", taps(point), 1,
         expected=(f"(phone) tab {tab}",), fits=True)
    for tab, point in LCD_TABS.items()
) + (
    Case("lcd-contact-page-fits", "tdeck", taps(LCD_FIRST_CONTACT), 1,
         expected=("(phone) page Anna Schmidt",), fits=True),
)

# Every simulated board opens the app on its first screen and fits it, as
# test_app_matrix.py checks the apps it knows. The OLED is too narrow for
# anything laid out for 240 px; there it need only open.
DEVICES = ("tdeck", "tdeck-pro", "tdeck-max", "tdisplay", "heltec-v3", "cardputer")
NARROW = ("heltec-v3",)


def in_order(log: str, expected: tuple[str, ...]) -> Optional[str]:
    """The first expected line missing, or out of order."""
    at = 0
    for line in expected:
        found = log.find(line, at)
        if found < 0:
            return line
        at = found + len(line)
    return None


def check_fits(stdout: str) -> None:
    if "Off-screen objects: " not in stdout:
        raise AssertionError("simulator did not report off-screen objects")
    offscreen = [line for line in stdout.splitlines() if line.startswith("Off screen: ")]
    if offscreen:
        raise AssertionError("wider than the screen: " + "; ".join(offscreen))


def run(simulator: Path, device: str, args: tuple[str, ...], steps: int,
        fits: bool) -> str:
    command = [str(simulator), "--headless", "--device", device,
               "--launch-app", "com.thistle.phone",
               "--timeout", str(3500 + 1500 * steps), *args]
    if fits:
        command.append("--report-offscreen")
    done = subprocess.run(command, text=True, capture_output=True, check=False, timeout=90)
    if done.returncode < 0:
        raise AssertionError(f"simulator died on signal {-done.returncode}")
    if done.returncode != 0:
        raise AssertionError(f"simulator exited {done.returncode}: {done.stderr.strip()[-300:]!r}")
    return done.stdout + done.stderr


def run_case(simulator: Path, case: Case) -> None:
    log = run(simulator, case.device, case.args, case.steps, case.fits)
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    if case.fits:
        check_fits(log)
    if case.after:
        why = case.after()
        if why:
            raise AssertionError(why)


def run_device(simulator: Path, device: str) -> None:
    log = run(simulator, device, (), 0, device not in NARROW)
    if "App launched: com.thistle.phone" not in log or "(phone) tab " not in log:
        raise AssertionError("the app did not open")
    if device not in NARROW:
        check_fits(log)


class CardState:
    """simulator/sdcard/phone as it was, to put back after each case."""

    def __init__(self) -> None:
        self.saved = Path(tempfile.mkdtemp(prefix="thistle-phone-test-"))
        self.existed = PHONE_DIR.is_dir()
        if self.existed:
            shutil.copytree(PHONE_DIR, self.saved / "phone")

    def restore(self) -> None:
        if PHONE_DIR.exists():
            shutil.rmtree(PHONE_DIR)
        if self.existed:
            shutil.copytree(self.saved / "phone", PHONE_DIR)

    def discard(self) -> None:
        shutil.rmtree(self.saved, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=REPO / "simulator/build/thistle_sim")
    parser.add_argument("-k", dest="only", default="",
                        help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    print("=== Phone app ===")
    state = CardState()
    failures = total = 0
    try:
        # No stars and no calls to begin with, whatever another run left.
        if PHONE_DIR.exists():
            shutil.rmtree(PHONE_DIR)
        for case in CASES:
            if args.only not in case.name:
                continue
            total += 1
            try:
                run_case(simulator, case)
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {case.name}: {exc}")
            else:
                print(f"PASS  {case.name}")
            finally:
                if PHONE_DIR.exists():
                    shutil.rmtree(PHONE_DIR)
        for device in DEVICES:
            name = f"opens-and-fits/{device}"
            if args.only not in name:
                continue
            total += 1
            try:
                run_device(simulator, device)
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                failures += 1
                print(f"FAIL  {name}: {exc}")
            else:
                print(f"PASS  {name}")
    finally:
        state.restore()
        state.discard()

    if failures:
        print(f"{failures}/{total} phone app tests failed")
        return 1
    print(f"All {total} phone app tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
