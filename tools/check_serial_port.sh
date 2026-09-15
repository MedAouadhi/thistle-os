#!/bin/bash
# Refuse to share a serial port.
#
# A USB-Serial-JTAG tty (/dev/ttyACM*) delivers each byte to exactly one of
# its readers. A second reader, such as a forgotten capture loop from another
# session, swallows a random subset of the chip's replies and esptool fails
# with "The chip stopped responding" or "device reports readiness to read but
# returned no data (... multiple access on port?)". Console captures come back
# empty or partial. Run this before idf.py flash / monitor or any capture.
#
# Holders are matched with bash's -ef test (same device node), not by path, so
# a process that opened the port through /dev/serial/by-id/... is still found.
#
# Usage: tools/check_serial_port.sh [/dev/ttyACM0]
# Exit 0 when nobody holds the port, 1 when someone does (holders are listed),
# 2 when the port does not exist.

set -u
PORT="${1:-/dev/ttyACM0}"

if [ ! -c "$PORT" ]; then
  echo "check_serial_port: $PORT is not a character device (unplugged?)" >&2
  exit 2
fi

found=0
for piddir in /proc/[0-9]*; do
  pid=${piddir#/proc/}
  [ "$pid" = "$$" ] && continue
  [ -r "$piddir/fd" ] || continue
  for fd in "$piddir"/fd/*; do
    if [ "$fd" -ef "$PORT" ]; then
      if [ $found -eq 0 ]; then
        echo "check_serial_port: $PORT is held by another process:" >&2
        found=1
      fi
      cmd=$(tr '\0' ' ' < "$piddir/cmdline" 2>/dev/null)
      cwd=$(readlink "$piddir/cwd" 2>/dev/null)
      printf '  PID %s (cwd %s): %s\n' "$pid" "${cwd:-?}" "$cmd" >&2
      break
    fi
  done
done

if [ $found -ne 0 ]; then
  echo "Stop those processes (kill <PID>) before touching $PORT." >&2
  exit 1
fi
echo "check_serial_port: $PORT is free"
