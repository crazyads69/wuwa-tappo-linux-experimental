#!/usr/bin/env python3 -I
"""Tiny Chrome-DevTools client (stdlib only) for the nested-session tests.
usage: cdp.py list | cdp.py eval <target-url-substring> '<js expression>' [--listen SECONDS]"""
import base64, json, os, socket, struct, sys, time, urllib.request
PORT = int(os.environ.get("CDP_PORT", "9222"))

def targets():
    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json"))

class WS:
    def __init__(self, url):
        host, rest = url[5:].split("/", 1)
        h, p = host.split(":")
        self.s = socket.create_connection((h, int(p)))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(f"GET /{rest} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        buf = b""
        while b"\r\n\r\n" not in buf: buf += self.s.recv(1)
        assert b" 101 " in buf.split(b"\r\n")[0], buf
    def send(self, obj):
        d = json.dumps(obj).encode(); mask = os.urandom(4)
        hdr = b"\x81" + (bytes([0x80 | len(d)]) if len(d) < 126 else b"\xfe" + struct.pack(">H", len(d)) if len(d) < 65536 else b"\xff" + struct.pack(">Q", len(d)))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(d)))
    def recv(self, timeout=None):
        self.s.settimeout(timeout)
        def rd(n):
            b = b""
            while len(b) < n:
                c = self.s.recv(n - len(b))
                if not c: raise EOFError
                b += c
            return b
        msg = b""
        while True:
            b0, b1 = rd(2); n = b1 & 0x7f
            if n == 126: n = struct.unpack(">H", rd(2))[0]
            elif n == 127: n = struct.unpack(">Q", rd(8))[0]
            msg += rd(n)
            if b0 & 0x80: return json.loads(msg)

def main():
    if sys.argv[1] == "list":
        for t in targets(): print(t["type"], t["url"], t["title"]); return
    sub, expr = sys.argv[2], sys.argv[3]
    t = next(t for t in targets() if sub in t["url"] and t["type"] == "page")
    ws = WS(t["webSocketDebuggerUrl"])
    ws.send({"id": 1, "method": "Runtime.enable"})
    ws.send({"id": 2, "method": "Runtime.evaluate", "params": {"expression": expr, "awaitPromise": True, "returnByValue": True}})
    end = time.time() + (float(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[4] == "--listen" else 0)
    while True:
        try: m = ws.recv(timeout=5)
        except Exception: break
        if m.get("id") == 2: print(json.dumps(m.get("result"))[:3000]); 
        elif m.get("method") == "Runtime.consoleAPICalled": print("console:", [a.get("value", a.get("description")) for a in m["params"]["args"]][:6])
        if m.get("id") == 2 and time.time() >= end: break
main()
