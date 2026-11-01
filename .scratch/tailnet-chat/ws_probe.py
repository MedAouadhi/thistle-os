"""Probe the Tailnet chat spike's echo: python3 ws_probe.py HOST [PORT] [--conns N] [--hold S] [--echoes K]

Times the TCP connect, the handshake's first byte, and each echo, on N
connections opened one after another and held open together for S seconds.
"""
import base64, os, socket, struct, sys, time

def ws_open(host, port, timeout=15):
    t0 = time.monotonic()
    s = socket.create_connection((host, port), timeout=timeout)
    t_conn = time.monotonic()
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /tailnet-chat/1 HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    first = s.recv(1)
    t_first = time.monotonic()
    if not first:
        raise RuntimeError("closed before answering")
    buf = first
    while b"\r\n\r\n" not in buf:
        d = s.recv(1024)
        if not d:
            raise RuntimeError("closed mid-answer: %r" % buf)
        buf += d
    status = buf.split(b"\r\n", 1)[0].decode()
    return s, status, (t_conn - t0) * 1000, (t_first - t0) * 1000

def ws_send(s, payload, op=1):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x80 | op])
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
    s.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

def ws_recv(s):
    def need(k):
        b = b""
        while len(b) < k:
            d = s.recv(k - len(b))
            if not d:
                raise RuntimeError("closed")
            b += d
        return b
    h = need(2)
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", need(2))[0]
    return h[0] & 0x0F, need(n)

def main():
    args = sys.argv[1:]
    host = args[0]
    port = int(args[1]) if len(args) > 1 and not args[1].startswith("--") else 4810
    opt = lambda name, d: float(args[args.index(name) + 1]) if name in args else d
    conns, hold, echoes = int(opt("--conns", 1)), opt("--hold", 0), int(opt("--echoes", 5))
    socks = []
    for i in range(conns):
        try:
            s, status, conn_ms, first_ms = ws_open(host, port)
        except Exception as e:
            print(f"conn {i}: {type(e).__name__}: {e}")
            continue
        rtts = []
        for k in range(echoes):
            msg = f"echo {i}.{k} " .encode() + os.urandom(8).hex().encode()
            t = time.monotonic()
            ws_send(s, msg)
            op, back = ws_recv(s)
            rtts.append((time.monotonic() - t) * 1000)
            assert back == msg, (back, msg)
        # one binary frame the size of an Opus call frame plus header
        frame = os.urandom(70)
        t = time.monotonic()
        ws_send(s, frame, op=2)
        op, back = ws_recv(s)
        bin_ms = (time.monotonic() - t) * 1000
        assert op == 2 and back == frame
        print(f"conn {i}: {status}; TCP connect {conn_ms:.0f} ms, first byte of the answer {first_ms:.0f} ms; "
              f"echo {', '.join(f'{r:.0f}' for r in rtts)} ms; 70 B binary {bin_ms:.0f} ms")
        socks.append(s)
    if hold:
        print(f"holding {len(socks)} open for {hold:.0f} s")
        time.sleep(hold)
    for s in socks:
        try:
            ws_send(s, b"", op=8)
            s.close()
        except OSError:
            pass

main()
