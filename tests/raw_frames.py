"""Talks to bserve with hand-built bytes, so the server is tested against the spec rather than against bcurl."""

import socket
import struct
import sys

PORT = int(sys.argv[1])
INDEX = open(sys.argv[2], "rb").read()
TABLE = [None, "host", "user-agent", "accept", "connection", "content-type",
         "content-length", "last-modified", "date", "server", "allow"]
failures = 0


def frame(ftype, flags, fid, payload=b""):
    n = len(payload)
    return bytes([n >> 16 & 0xFF, n >> 8 & 0xFF, n & 0xFF, ftype, flags, 0]) + struct.pack(">H", fid) + payload


def s16(b):
    return struct.pack(">H", len(b)) + b


def request(fid, path, method=1, extra=b"", flags=1):
    return frame(1, flags, fid, bytes([method]) + s16(path) + extra)


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("server closed the connection")
        buf += chunk
    return buf


def read_frame(sock):
    h = read_exact(sock, 8)
    n = h[0] << 16 | h[1] << 8 | h[2]
    return h[3], h[4], struct.unpack(">H", h[6:8])[0], read_exact(sock, n)


def parse_headers(p):
    headers, i = {}, 0
    while i < len(p):
        idx = p[i]
        i += 1
        if idx == 0:
            n = struct.unpack(">H", p[i:i + 2])[0]
            name = p[i + 2:i + 2 + n].decode()
            i += 2 + n
        else:
            name = TABLE[idx]
        n = struct.unpack(">H", p[i:i + 2])[0]
        headers[name] = p[i + 2:i + 2 + n].decode()
        i += 2 + n
    return headers


def read_response(sock):
    while True:
        ftype, flags, fid, p = read_frame(sock)
        if ftype == 2:
            break
    status = struct.unpack(">H", p[:2])[0]
    headers = parse_headers(p[2:])
    body = b""
    while not flags & 1:
        ftype, flags, _, d = read_frame(sock)
        if ftype == 3:
            body += d
    return fid, status, headers, body


def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    return s


def check(name, ok, detail=""):
    global failures
    if ok:
        print(f"ok - {name}")
    else:
        failures += 1
        print(f"not ok - {name} {detail}")


with connect() as s:
    s.sendall(frame(1, 1, 1, b"\x01\x00\x50/x"))
    fid, status, _, _ = read_response(s)
    check("400 when path length runs past the payload", (fid, status) == (1, 400), (fid, status))
    s.sendall(request(2, b"/index.html"))
    fid, status, _, body = read_response(s)
    check("connection survives the 400", (fid, status, body) == (2, 200, INDEX), (fid, status))

with connect() as s:
    s.sendall(frame(0x7F, 0, 0, b"some v2 frame") + frame(0xEE, 0xFF, 9, b"") + request(1, b"/index.html"))
    _, status, _, body = read_response(s)
    check("unknown frame types are skipped", (status, body) == (200, INDEX), status)

with connect() as s:
    s.sendall(request(1, b"/index.html", extra=b"\x42" + s16(b"reserved index") + b"\x00" + s16(b"x-test") + s16(b"1")))
    _, status, _, _ = read_response(s)
    check("unknown header index and literal names are accepted", status == 200, status)

with connect() as s:
    s.sendall(request(1, b"/index.html", flags=0) + frame(3, 1, 1, b"ignored body") + request(2, b"/index.html"))
    r1, r2 = read_response(s), read_response(s)
    check("request DATA frames are ignored", (r1[1], r2[0], r2[1]) == (200, 2, 200), (r1[:2], r2[:2]))

cases = [
    ("404 for a missing file", request(1, b"/nope.html"), 404),
    ("404 for a path that escapes the root", request(1, b"/../../../../../../../etc/passwd"), 404),
    ("400 for a path without a leading slash", request(1, b"index.html"), 400),
    ("400 for request id 0", request(0, b"/index.html"), 400),
    ("400 for an empty literal header name", request(1, b"/", extra=b"\x00" + s16(b"") + s16(b"v")), 400),
    ("400 for a RESPONSE frame sent to the server", frame(2, 1, 1, b"\x00\xc8"), 400),
    ("405 for POST", request(1, b"/index.html", method=3), 405),
    ("501 for an unknown method", request(1, b"/index.html", method=9), 501),
    ("200 for / via index.html", request(1, b"/?q=1"), 200),
]
for name, raw, want in cases:
    with connect() as s:
        s.sendall(raw)
        _, status, headers, _ = read_response(s)
        check(name, status == want, status)
        if want == 405:
            check("405 carries allow", headers.get("allow") == "GET, HEAD", headers)

with connect() as s:
    s.sendall(request(1, b"/index.html", method=2))
    _, status, headers, body = read_response(s)
    check("HEAD sends headers and no body",
          (status, body, headers.get("content-length")) == (200, b"", str(len(INDEX))), (status, headers))

with connect() as s:
    s.sendall(b"\x00\x00")
with connect() as s:
    s.sendall(request(1, b"/index.html"))
    check("server survives a truncated header", read_response(s)[1] == 200)

sys.exit(1 if failures else 0)
