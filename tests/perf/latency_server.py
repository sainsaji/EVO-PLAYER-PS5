#!/usr/bin/env python3
"""latency_server.py - an HTTP/1.1 test server that behaves like a far server.

    python3 tests/perf/latency_server.py PORT RTT_MS [nagle]

Every new connection waits one RTT before it is served (the TCP handshake a
real link costs), and every response waits one RTT (request out, response
back). Idle keep-alive connections are closed after 1 s, so a client that
reuses a stale one is caught.

  /json      2 KB JSON with Content-Length
  /chunked   the same body, chunked, with a trailer
  /close     Content-Length plus "Connection: close"
  /empty     204, no body
  /stats     how many connections were accepted so far
"""
import socket
import socketserver
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
RTT = float(sys.argv[2]) / 1000.0
NAGLE = len(sys.argv) > 3 and sys.argv[3] == "nagle"
BODY = (b'{"items":[' + b",".join(b'{"id":%d,"name":"item %d"}' % (i, i) for i in range(80)) + b"]}")
ACCEPTED = 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 1.0                      # idle keep-alive connections close after 1 s

    def setup(self):
        super().setup()
        # nginx, Kestrel (Jellyfin/Emby) and Apache set TCP_NODELAY; "nagle"
        # leaves it off to show the worst case (header and body as two writes
        # meet the client's delayed ACK on a kept-open connection).
        if not NAGLE:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *a):
        pass

    def do_GET(self):
        time.sleep(RTT)
        if self.path == "/stats":
            body = b"%d" % ACCEPTED
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Done")
            self.end_headers()
            for i in range(0, len(BODY), 700):
                part = BODY[i:i + 700]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\nX-Done: yes\r\n\r\n")
        elif self.path == "/close":
            self.send_response(200)
            self.send_header("Content-Length", str(len(BODY)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(BODY)
            self.close_connection = True
        elif self.path == "/empty":
            self.send_response(204)
            self.end_headers()
        else:
            self.send_error(404)


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def get_request(self):
        global ACCEPTED
        conn = super().get_request()
        ACCEPTED += 1
        time.sleep(RTT)                # the handshake
        return conn


socketserver.TCPServer.allow_reuse_address = True
Server(("127.0.0.1", PORT), Handler).serve_forever()
