#!/usr/bin/env python3
"""Serves a directory over HTTP with range requests and simulated faults.

Used by tool/regression.sh to test network behaviour:

  --rate KBPS          limit every response to KBPS kilobytes per second
  --stall-at OFFSET    the first response to reach file offset OFFSET stops
                       sending there...
  --stall-for SECONDS  ...for this long, then continues
  --drop-at OFFSET     the first response to reach file offset OFFSET closes
                       its connection there
  --refuse-after-drop  after the drop, every request fails with 503
  --refuse-after-requests N  after N requests, every later one fails with 403
  --no-ranges          ignore Range headers: every response is the whole file
  --redirect FROM TO   answer requests for path FROM with a 302 to path TO

Prints "port N" once listening (pass --port 0 for a free port).
"""
import argparse
import http.server
import os
import re
import socketserver
import sys
import threading
import time


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.stalled = False
        self.dropped = False
        self.requests = 0

    def claim(self, attribute):
        # True for the one caller that flips |attribute| first.
        with self.lock:
            if getattr(self, attribute):
                return False
            setattr(self, attribute, True)
            return True


def make_handler(root, args, state):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *values):
            if args.verbose:
                sys.stderr.write("media_server: " + fmt % values + "\n")

        def do_HEAD(self):
            self.serve(head=True)

        def do_GET(self):
            self.serve(head=False)

        def serve(self, head):
            if args.refuse_after_drop and state.dropped:
                self.send_error(503)
                return
            if args.redirect and self.path.split("?")[0] == args.redirect[0]:
                self.send_response(302)
                self.send_header("Location", args.redirect[1])
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            path = os.path.join(root, self.path.lstrip("/").split("?")[0])
            if not os.path.isfile(path):
                self.send_error(404)
                return
            if args.refuse_after_requests is not None:
                with state.lock:
                    state.requests += 1
                    refused = state.requests > args.refuse_after_requests
                if refused:
                    self.send_error(403)
                    return
            size = os.path.getsize(path)
            start, end = 0, size - 1
            match = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
            if match and (match.group(1) or match.group(2)) and not args.no_ranges:
                if match.group(1):
                    start = int(match.group(1))
                    if match.group(2):
                        end = min(int(match.group(2)), size - 1)
                else:
                    start = max(size - int(match.group(2)), 0)
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            else:
                self.send_response(200)
            if not args.no_ranges:
                self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            self.end_headers()
            if head:
                return

            sent = 0
            began = time.monotonic()
            offset = start
            with open(path, "rb") as f:
                f.seek(start)
                remaining = end - start + 1
                while remaining > 0:
                    chunk = f.read(min(16384, remaining))
                    if not chunk:
                        break
                    try:
                        if (args.drop_at is not None and offset <= args.drop_at < offset + len(chunk)
                                and state.claim("dropped")):
                            self.wfile.write(chunk[: args.drop_at - offset])
                            self.wfile.flush()
                            self.close_connection = True
                            self.connection.shutdown(2)
                            return
                        if (args.stall_at is not None and offset <= args.stall_at < offset + len(chunk)
                                and state.claim("stalled")):
                            cut = args.stall_at - offset
                            self.wfile.write(chunk[:cut])
                            self.wfile.flush()
                            time.sleep(args.stall_for)
                            chunk = chunk[cut:]
                            sent += cut
                            offset += cut
                            remaining -= cut
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        return
                    sent += len(chunk)
                    offset += len(chunk)
                    remaining -= len(chunk)
                    if args.rate:
                        due = began + sent / (args.rate * 1024)
                        delay = due - time.monotonic()
                        if delay > 0:
                            time.sleep(delay)

    return Handler


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer.server_bind looks up the host's name (socket.getfqdn),
        # a reverse DNS query that can hang on CI runners. Nothing uses it.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--rate", type=float, default=0)
    parser.add_argument("--stall-at", type=int)
    parser.add_argument("--stall-for", type=float, default=5)
    parser.add_argument("--drop-at", type=int)
    parser.add_argument("--refuse-after-drop", action="store_true")
    parser.add_argument("--refuse-after-requests", type=int)
    parser.add_argument("--no-ranges", action="store_true")
    parser.add_argument("--redirect", nargs=2, metavar=("FROM", "TO"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    server = Server(("127.0.0.1", args.port),
                    make_handler(args.root, args, State()))
    print(f"port {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
