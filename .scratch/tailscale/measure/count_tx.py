#!/usr/bin/env python3
"""Count what the tailnet sends and receives, per minute, from ml_count lines.

    count_tx.py LOG [--skip S] [--until S]

--skip drops the first S seconds after the first ml_count line (start-up);
--until stops S seconds after it. Bursts: packets less than 2 s apart are one
burst, which is closer to what wakes a modem than the packets are.
"""
import argparse
import collections
import re

LINE = re.compile(r"[IWE] \((\d+)\) ml_count: (tx|rx) (\w+) (.*)")


def label(direction, path, rest):
    kv = dict(re.findall(r"(\w+)=(\w+)", rest))
    if path == "ctrl":
        h2 = int(kv.get("h2", -1))
        fl = int(kv.get("fl", 0))
        name = {0: "DATA", 1: "HEADERS (MapRequest)" if direction == "tx" else "HEADERS", 4: "SETTINGS",
                6: "PING", 8: "WINDOW_UPDATE"}.get(h2, f"h2 type {h2}")
        if h2 == 6:
            name = "PING ack" if fl & 1 else "PING"
        if h2 == 0 and direction == "rx":
            name += f" stream {kv.get('sid')}"
        return f"control {name}"
    if path == "derp":
        t = int(kv.get("type", -1))
        inner = kv.get("inner", "00")
        if t in (4, 5):
            what = "DISCO" if inner == "54" else f"WireGuard type {int(inner, 16)}"
            return f"DERP {'send' if t == 4 else 'recv'} packet, {what}"
        return "DERP " + {6: "KeepAlive", 7: "NotePreferred", 8: "PeerGone", 0x12: "Ping", 0x13: "Pong"}.get(t, f"frame {t}")
    if path == "udp":
        parts = rest.split()
        if direction == "rx":
            if parts[0] == "sock4":
                first = kv.get("first", "0000")
                if first == "5453":
                    kind = "DISCO"
                elif first == "0101":
                    kind = "STUN answer"
                else:
                    kind = f"WireGuard type {int(first[:2], 16)}"
                return f"UDP {kind}"
            return "UDP STUN (stun socket)"
        return f"UDP {parts[0]}" + (f" {parts[1]}" if parts[0] == "stun" else "")
    return f"{path} {rest}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=float, default=0)
    ap.add_argument("--until", type=float, default=None)
    args = ap.parse_args()

    events = []
    with open(args.log, "rb") as f:
        for raw in f:
            m = LINE.search(raw.decode("utf-8", "replace"))
            if m:
                events.append((int(m.group(1)) / 1000.0, m.group(2), label(m.group(2), m.group(3), m.group(4))))
    if not events:
        print("no ml_count lines")
        return
    t0 = events[0][0] + args.skip
    t1 = events[-1][0] if args.until is None else events[0][0] + args.until
    events = [e for e in events if t0 <= e[0] <= t1]
    minutes = (t1 - t0) / 60.0
    print(f"window {t1 - t0:.0f} s ({minutes:.2f} min), log time {t0:.0f}..{t1:.0f} s")

    for direction in ("tx", "rx"):
        counts = collections.Counter(l for t, d, l in events if d == direction)
        print(f"\n{direction}: {sum(counts.values())} packets, {sum(counts.values()) / minutes:.1f}/min")
        for name, n in counts.most_common():
            print(f"  {n:5d}  {n / minutes:6.2f}/min  {name}")

    for which in ("tx", "any"):
        times = [t for t, d, _ in events if which == "any" or d == which]
        bursts = []
        for t in times:
            if not bursts or t - bursts[-1][1] > 2.0:
                bursts.append([t, t])
            else:
                bursts[-1][1] = t
        gaps = [b[0] - a[1] for a, b in zip(bursts, bursts[1:])]
        if gaps:
            gaps.sort()
            print(f"\n{which} bursts: {len(bursts)}, {len(bursts) / minutes:.2f}/min; "
                  f"gap between bursts min {gaps[0]:.1f} s, median {gaps[len(gaps) // 2]:.1f} s, max {gaps[-1]:.1f} s")


if __name__ == "__main__":
    main()
