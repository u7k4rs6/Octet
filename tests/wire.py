#!/usr/bin/env python3
"""Independent Octet implementation used to test bserve and bcurl from the
other side of the wire. Written from SPEC.md only; shares no code with src/.

usage: wire.py server-tests <port>
       wire.py client-tests <path-to-bcurl>
"""
import socket
import struct
import subprocess
import sys
import threading
import time

REQUEST, RESPONSE, DATA = 1, 2, 3
END = 0x01
STATIC = [None, ":method", ":path", "host", "user-agent", "accept",
          ":status", "content-length", "content-type", "server", "date"]


def frame(ftype, flags, payload, version=1, length=None):
    n = len(payload) if length is None else length
    return struct.pack(">I", n)[1:] + bytes([ftype, flags, version]) + payload


def hblock(headers, count=None):
    out = bytes([len(headers) if count is None else count])
    for name, value in headers:
        v = value.encode() if isinstance(value, str) else value
        if isinstance(name, int):
            out += bytes([0x80 | name])
        else:
            out += b"\x00" + bytes([len(name)]) + name.encode()
        out += struct.pack(">H", len(v)) + v
    return out


def request(path, extra=()):
    return frame(REQUEST, END, hblock([(1, "GET"), (2, path), (3, "test")] + list(extra)))


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("closed after %d of %d bytes" % (len(buf), n))
        buf += chunk
    return buf


def read_frame(s):
    h = recv_exact(s, 6)
    n = struct.unpack(">I", b"\x00" + h[:3])[0]
    return h[3], h[4], h[5], recv_exact(s, n)


def parse_hblock(p):
    count, off, out = p[0], 1, {}
    for _ in range(count):
        form = p[off]; off += 1
        if form & 0x80:
            name = STATIC[form & 0x7F]
        else:
            nl = p[off]; off += 1
            name = p[off:off + nl].decode(); off += nl
        vl = struct.unpack(">H", p[off:off + 2])[0]; off += 2
        out[name] = p[off:off + vl].decode(); off += vl
    assert off == len(p), "trailing bytes"
    return out


def read_response(s):
    t, flags, v, p = read_frame(s)
    assert (t, v) == (RESPONSE, 1), (t, v)
    hdrs, body = parse_hblock(p), b""
    while not flags & END:
        t, flags, v, p = read_frame(s)
        assert t == DATA, t
        body += p
    return int(hdrs[":status"]), hdrs, body


def closed(s):
    s.settimeout(2)
    try:
        return s.recv(1) == b""
    except (ConnectionResetError, socket.timeout):
        return True


FAILS = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        FAILS.append(name)


# ---- server tests -------------------------------------------------------

def server_tests(port):
    def conn():
        s = socket.create_connection(("127.0.0.1", port))
        s.settimeout(5)
        return s

    s = conn()
    s.sendall(request("/hello.txt"))
    st, h, body = read_response(s)
    check("200 with content-length and content-type",
          st == 200 and h["content-length"] == str(len(body)) and "text/plain" in h["content-type"])
    s.sendall(request("/missing"))
    st, h, body = read_response(s)
    check("404 on the same connection, no body", st == 404 and body == b"")
    s.sendall(request("/hello.txt") + request("/"))
    a, b = read_response(s), read_response(s)
    check("two pipelined requests answered in order", a[0] == 200 and b[0] == 200 and b"<html" in b[2])
    s.close()

    s = conn()
    s.sendall(frame(0x7F, 0xFF, b"future stuff") + frame(0x42, 0, b"") + request("/hello.txt"))
    check("unknown frame types are skipped, connection stays up", read_response(s)[0] == 200)
    s.close()

    s = conn()
    data = request("/hello.txt")
    for i in range(len(data)):              # one byte per TCP write
        s.sendall(data[i:i + 1])
        time.sleep(0.002)
    check("request split into 1-byte reads", read_response(s)[0] == 200)
    s.close()

    s = conn()
    s.sendall(frame(REQUEST, END, hblock([("x-trace", "abc"), (1, "GET"), (":path", "/hello.txt")])))
    check("literal names accepted, including a literal :path", read_response(s)[0] == 200)
    s.close()

    malformed = {
        "version 2": frame(REQUEST, END, hblock([(1, "GET"), (2, "/")]), version=2),
        "reserved flag bit": frame(REQUEST, END | 0x02, hblock([(1, "GET"), (2, "/")])),
        "REQUEST without END": frame(REQUEST, 0, hblock([(1, "GET"), (2, "/")])),
        "length over 64 KiB cap": frame(REQUEST, END, b"", length=70000),
        "indexed name 0": frame(REQUEST, END, bytes([2, 0x81, 0, 3]) + b"GET" + bytes([0x80, 0, 1]) + b"/"),
        "indexed name 11": frame(REQUEST, END, hblock([(1, "GET"), (11, "/")])),
        "count runs past length": frame(REQUEST, END, hblock([(1, "GET"), (2, "/")], count=3)),
        "value length runs past frame": frame(REQUEST, END, bytes([1, 0x82, 0xFF, 0xFF]) + b"/"),
        "trailing bytes": frame(REQUEST, END, hblock([(1, "GET"), (2, "/")]) + b"\x00"),
        "missing :path": frame(REQUEST, END, hblock([(1, "GET")])),
        "missing :method": frame(REQUEST, END, hblock([(2, "/")])),
        "method POST": frame(REQUEST, END, hblock([(1, "POST"), (2, "/")])),
        "non-printable value": frame(REQUEST, END, hblock([(1, "GET"), (2, "/\x01")])),
        "uppercase literal name": frame(REQUEST, END, hblock([(1, "GET"), (2, "/"), ("X-Up", "1")])),
        "client sends DATA": frame(DATA, END, b"hi"),
    }
    for name, raw in malformed.items():
        s = conn()
        s.sendall(raw)
        try:
            st = read_response(s)[0]
        except Exception:
            st = None
        check("400 and close: " + name, st == 400 and closed(s))
        s.close()

    for path in ["/../etc/passwd", "/a/../hello.txt", "//etc/passwd", "hello.txt",
                 "/escape", "/sub", "/sub/"]:
        s = conn()
        s.sendall(request(path))
        check("404 for " + path, read_response(s)[0] == 404)
        s.close()
    s = conn()
    s.sendall(request("/inside"))
    check("symlink that stays inside the root is served", read_response(s)[0] == 200)
    s.close()


# ---- client tests: a fake server feeds bcurl crafted responses -----------

def fake_server(reply):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(1)
    port = ls.getsockname()[1]
    accepted = []

    def run():
        c, _ = ls.accept()
        accepted.append(c)
        try:
            read_frame(c)
            c.sendall(reply)
        finally:
            time.sleep(0.2)
            c.close()
            ls.close()

    threading.Thread(target=run, daemon=True).start()
    return port


def resp(status, body=b"", split=1, extra=b""):
    hb = hblock([(6, str(status)), (7, str(len(body)))])
    if not body:
        return extra + frame(RESPONSE, END, hb)
    out = extra + frame(RESPONSE, 0, hb)
    step = max(1, -(-len(body) // split))
    chunks = [body[i:i + step] for i in range(0, len(body), step)]
    for i, c in enumerate(chunks):
        out += frame(DATA, END if i == len(chunks) - 1 else 0, c)
    return out


def client_tests(bcurl):
    def run(reply, path="/x"):
        port = fake_server(reply)
        p = subprocess.run([bcurl, "127.0.0.1:%d%s" % (port, path)],
                           capture_output=True, timeout=10)
        return p.returncode, p.stdout

    code, out = run(resp(200, b"abcdefghij", split=4, extra=frame(0x99, 0, b"v2 only")))
    check("client: unknown frame skipped, DATA split across 4 frames", code == 0 and out == b"abcdefghij")
    check("client: 404 exits 1", run(resp(404))[0] == 1)
    check("client: 500 exits 1", run(resp(500))[0] == 1)
    check("client: server closes before END exits 2",
          run(frame(RESPONSE, 0, hblock([(6, "200"), (7, "5")])) + frame(DATA, 0, b"ab"))[0] == 2)
    check("client: bad version exits 2", run(frame(RESPONSE, END, hblock([(6, "200")]), version=9))[0] == 2)
    check("client: reserved flag exits 2", run(frame(RESPONSE, END | 4, hblock([(6, "200")])))[0] == 2)
    check("client: DATA before RESPONSE exits 2", run(frame(DATA, END, b"x"))[0] == 2)
    check("client: DATA beyond content-length exits 2",
          run(frame(RESPONSE, 0, hblock([(6, "200"), (7, "1")])) + frame(DATA, END, b"xyz"))[0] == 2)
    check("client: body shorter than content-length exits 2",
          run(frame(RESPONSE, 0, hblock([(6, "200"), (7, "9")])) + frame(DATA, END, b"xyz"))[0] == 2)
    check("client: missing :status exits 2", run(frame(RESPONSE, END, hblock([(9, "x")])))[0] == 2)


if __name__ == "__main__":
    if sys.argv[1] == "server-tests":
        server_tests(int(sys.argv[2]))
    else:
        client_tests(sys.argv[2])
    sys.exit(1 if FAILS else 0)
