#!/usr/bin/env python3
"""Capture a USB-Serial-JTAG board's boot into a file.

Opens the port with DTR and RTS held low, the way esp-idf-monitor holds it.
Opening it that way RESETS the board (rst:0x15, verified three times out of
three on the T-Deck Max), so a capture always starts with a fresh boot, and it
receives the whole boot: ROM banner, bootloader, ``cpu_start``, application.
A ``cat`` reader on the same port does not reset the board but also does not
receive the application's boot output. Use this for boots and ``stty -hupcl
clocal`` + ``cat`` for watching a board you must not disturb (see CLAUDE.md).

    tools/serial_capture.py boot.log                # reset, capture until Ctrl-C
    tools/serial_capture.py boot.log --seconds 30   # reset, capture 30 s

Refuses to start if another process already holds the port: a second reader
steals bytes from the first, and from esptool.

Needs pyserial; run from the ESP-IDF environment (``. export.sh``).
"""

import argparse
import os
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))


def port_is_free(port):
    check = os.path.join(HERE, "check_serial_port.sh")
    return subprocess.call([check, port]) == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logfile")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=None,
                    help="stop after this many seconds (default: run until Ctrl-C)")
    args = ap.parse_args()

    if not port_is_free(args.port):
        return 1

    p = serial.Serial()
    p.port = args.port
    p.baudrate = args.baud
    p.dtr = False  # set before open(); with the lines high the boot is not received
    p.rts = False
    p.timeout = 0.5
    p.open()  # this resets the board; the capture starts with its boot

    t0 = time.time()
    n = 0
    with open(args.logfile, "ab", buffering=0) as out:
        try:
            while args.seconds is None or time.time() - t0 < args.seconds:
                data = p.read(4096)
                if data:
                    out.write(data)
                    n += len(data)
        except KeyboardInterrupt:
            pass
        finally:
            p.close()
    print(f"serial_capture: {n} bytes in {time.time() - t0:.1f} s -> {args.logfile}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
