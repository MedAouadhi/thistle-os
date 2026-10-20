#!/usr/bin/env python3
"""The To-do app on the simulated T-Deck Max: typing and Enter adds a task,
a tap ticks one off, holding one removes it, Clear done clears what is
ticked off, and the keys do the same; every change is in the card's todo.md,
a Markdown checklist, by the time the app has made it.

The app logs each change, as "(todo) added; 2 tasks, 2 to do", and how many
tasks it read when it opened.

--type delivers its keys in one go into the keyboard's 32-event queue, so a
run types one short task: a case that needs a list starts from a todo.md.

Coordinates are the app's own layout on the T-Deck Max (240 x 320, the
status bar 24 px; todo_ui.c): the title bar is y 24 to 54, Clear done at its
right end, the field y 62 to 94, and the first task's row, one line high,
y 100 to 132.

Every case runs on a scratch copy of simulator/sdcard.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

SDCARD = Path(__file__).resolve().parent.parent / "sdcard"

FIRST_ROW = "120,116"
CLEAR_DONE = "188,38"
# The field keeps the keyboard: the simulator says where a text field's
# cursor is only when one has it.
FIELD_HAS_KEYBOARD = "Focused cursor: "

TWO = "- [ ] Call Anna\n- [ ] Buy milk\n"


@dataclass(frozen=True)
class Case:
    name: str
    args: tuple[str, ...]
    # What todo.md holds before the run, or None for none.
    before: Optional[str]
    # What it holds after.
    after: str
    expected: tuple[str, ...] = ()
    absent: tuple[str, ...] = ()
    timeout_ms: int = 4000


CASES = (
    Case("typing-and-enter-adds-a-task",
         ("--type", "Buy milk\n"), None,
         "- [ ] Buy milk\n",
         expected=("(todo) no list yet", "(todo) added; 1 tasks, 1 to do",
                   'Focused text: ""', FIELD_HAS_KEYBOARD)),
    Case("a-new-task-goes-first",
         ("--type", "Call Anna\n"), "- [ ] Buy milk\n",
         TWO,
         expected=("(todo) 1 tasks, 1 to do", "(todo) added; 2 tasks, 2 to do")),
    Case("enter-with-nothing-typed-adds-nothing",
         ("--type", "   \n"), "- [ ] Buy milk\n",
         "- [ ] Buy milk\n",
         absent=("(todo) added",)),
    Case("a-tap-ticks-a-task-off-and-leaves-the-keyboard-in-the-field",
         ("--tap", FIRST_ROW, "--type", "x"), TWO,
         "- [x] Call Anna\n- [ ] Buy milk\n",
         expected=("(todo) ticked off; 1 to do", 'Focused text: "x"')),
    Case("a-second-tap-puts-it-back",
         ("--tap", FIRST_ROW, "--tap", FIRST_ROW), TWO,
         TWO,
         expected=("(todo) ticked off; 1 to do", "(todo) not done after all; 2 to do"),
         timeout_ms=5500),
    Case("holding-a-task-removes-it-and-ticks-off-nothing",
         ("--tap", FIRST_ROW, "--tap-hold", "900"), TWO,
         "- [ ] Buy milk\n",
         expected=("(todo) removed; 1 tasks, 1 to do",),
         absent=("ticked off",),
         timeout_ms=5000),
    Case("clear-done-removes-what-is-ticked-off",
         ("--tap", CLEAR_DONE), "- [x] Call Anna\n- [ ] Buy milk\n- [x] Pay the rent\n",
         "- [ ] Buy milk\n",
         expected=("(todo) cleared 2 done; 1 to do",)),
    Case("tab-then-space-ticks-off-the-first-task",
         ("--type", "\t "), TWO,
         "- [x] Call Anna\n- [ ] Buy milk\n",
         expected=("(todo) ticked off; 1 to do",)),
    Case("tab-then-enter-ticks-off-the-first-task",
         ("--type", "\t\n"), TWO,
         "- [x] Call Anna\n- [ ] Buy milk\n",
         expected=("(todo) ticked off; 1 to do",),
         absent=("(todo) added",)),
    Case("tab-then-backspace-removes-the-first-task",
         ("--type", "\t\b"), TWO,
         "- [ ] Buy milk\n",
         expected=("(todo) removed; 1 tasks, 1 to do",)),
    Case("typing-in-the-list-goes-to-the-field",
         ("--type", "\tab"), TWO,
         TWO,
         expected=('Focused text: "ab"', FIELD_HAS_KEYBOARD)),
    Case("a-list-edited-by-hand-is-read-and-saved-tidy",
         ("--type", "Tea\n"),
         "# Shopping\n\n- [X] Bread\n* Eggs\nJam\n",
         "- [ ] Tea\n- [x] Bread\n- [ ] Eggs\n- [ ] Jam\n",
         expected=("(todo) 3 tasks, 2 to do", "(todo) added; 4 tasks, 3 to do")),
    Case("a-full-list-takes-no-more",
         ("--type", "One more\n"),
         "".join(f"- [ ] Task {i}\n" for i in range(50)),
         "".join(f"- [ ] Task {i}\n" for i in range(50)),
         expected=("(todo) 50 tasks, 50 to do", "toast: The list is full: clear what is done"),
         absent=("(todo) added",)),
)


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
    with tempfile.TemporaryDirectory(prefix="thistle-todo-") as tmp:
        card = Path(tmp) / "card"
        shutil.copytree(SDCARD, card)
        todo = card / "todo.md"
        todo.unlink(missing_ok=True)
        (card / "todo.md.tmp").unlink(missing_ok=True)
        if case.before is not None:
            todo.write_text(case.before)
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card))

        command = [str(simulator), "--headless", "--device", "tdeck-max",
                   "--launch-app", "com.thistle.todo",
                   "--timeout", str(case.timeout_ms), *case.args]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=case.timeout_ms / 1000 + 30, env=env)
        after = todo.read_text() if todo.exists() else None
    log = run.stdout + run.stderr
    if run.returncode != 0:
        raise AssertionError(f"simulator exited {run.returncode}")
    if "App launched: com.thistle.todo" not in log:
        raise AssertionError("the app did not launch")
    missing = in_order(log, case.expected)
    if missing:
        raise AssertionError(f"no {missing!r}, in order")
    for line in case.absent:
        if line in log:
            raise AssertionError(f"unexpected {line!r}")
    if after != case.after:
        raise AssertionError(f"todo.md holds {after!r}, not {case.after!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=Path("build/thistle_sim"))
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()

    simulator = args.sim.resolve()
    if not simulator.is_file():
        parser.error(f"simulator not found: {simulator}")

    cases = [c for c in CASES if not args.only or args.only in c.name]
    print("=== To-do ===")
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
        print(f"{failures}/{len(cases)} To-do tests failed")
        return 1
    print(f"All {len(cases)} To-do tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
