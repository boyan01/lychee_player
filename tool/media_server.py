#!/usr/bin/env python3
"""Serves a directory over HTTP with range requests and simulated faults.

Used by tool/regression.sh to test network behaviour:

  --rate KBPS          limit every response to KBPS kilobytes per second
  --stall-at OFFSET    the first response to reach file offset OFFSET stops
                       sending there...
  --stall-for SECONDS  ...for this long, then continues
  --stall-skip N       the first N file responses (in the order the
                       requests came) never stall, wherever they reach: a
                       client that looks at the head on one connection and
                       reads on another
  --drop-at OFFSET     the first response to reach file offset OFFSET closes
                       its connection there
  --refuse-after-drop  after the drop, every request fails with 503
  --refuse-after-requests N  after N requests, every later one fails with 403
  --refuse-after-seconds S   requests S seconds after the first one fail
                       with 403, but for one that starts where an earlier
                       one's range ended (a client reading on in bounded
                       ranges): a jump fails, the read going on does not
  --no-ranges          ignore Range headers: every response is the whole file
  --latency SECONDS    wait this long before answering each file request,
                       as a distant server's round trips do
  --redirect FROM TO   answer requests for path FROM with a 302 to path TO
  --status PATH CODE   answer requests for path PATH with HTTP status CODE.
                       Repeatable
  --require-header NAME VALUE  refuse requests without header NAME set to
                       VALUE with 403, redirects included, and log them.
                       Repeatable
  --live PATH VOD      answer requests for path PATH with a live HLS
                       playlist made from the VOD playlist at path VOD (in
                       the same directory): its first three segments at the
                       first request, one more each time the previous one
                       has played in real time, a window of four, and
                       EXT-X-ENDLIST once all are out. Repeatable (the
                       renditions of a master playlist), on one clock
  --live-start N       live playlists start with N segments out (default 3)
  --live-window N      live playlists keep a window of N segments (default
                       4); 0 keeps all, as PLAYLIST-TYPE EVENT
  --live-flv PATH FLV  answer requests for path PATH with the FLV file at path
                       FLV as an HTTP-FLV live stream: chunked, without a
                       length or ranges, each tag sent once its time has come
                       (from the first request on). A later request gets the
                       header and the sequence headers, then the tags from
                       the keyframe before the time that has come, as a live
                       server's GOP cache sends them. The stream ends with
                       the file
  --live-flv-burst S   the first request gets the file's first S seconds at
                       once, as a server sends what it holds
  --live-flv-drop-after S  the first live FLV response breaks off (a TCP
                       reset) S seconds after its request
  --live-flv-restart   a later request's tag times start over from 0, as
                       after an encoder restart
  --live-flv-uptime MS the stream's tag times are MS later than the
                       file's, as those of a stream live for a while

Prints "port N" once listening (pass --port 0 for a free port).
"""
import argparse
import http.server
import os
import re
import socket
import socketserver
import struct
import sys
import threading
import time


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.stalled = False
        self.dropped = False
        self.requests = 0
        # File responses (GET) so far, for --stall-skip.
        self.responses = 0
        self.first_request = None
        # Where the ranges asked for so far end, per path.
        self.range_ends = set()
        self.live_start = None
        self.flv_start = None
        self.flv_requests = 0

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

        def setup(self):
            super().setup()
            # One line per connection, so a client's reuse shows.
            if args.verbose:
                sys.stderr.write("media_server: connection from port %d\n"
                                 % self.client_address[1])

        def do_HEAD(self):
            self.serve(head=True)

        def do_GET(self):
            self.serve(head=False)

        def serve(self, head):
            for name, value in args.require_header or []:
                if self.headers.get(name) != value:
                    sys.stderr.write("media_server: %s without %s: %s\n"
                                     % (self.path, name, value))
                    self.send_error(403)
                    return
            if args.refuse_after_drop and state.dropped:
                self.send_error(503)
                return
            for status_path, code in args.status or []:
                if self.path.split("?")[0] == status_path:
                    self.send_error(int(code))
                    return
            if args.redirect and self.path.split("?")[0] == args.redirect[0]:
                self.send_response(302)
                self.send_header("Location", args.redirect[1])
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            for live, vod in args.live or []:
                if self.path.split("?")[0] == live:
                    self.serve_live(vod, head)
                    return
            if args.live_flv and self.path.split("?")[0] == args.live_flv[0]:
                self.serve_live_flv(head)
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
            if args.latency:
                time.sleep(args.latency)
            size = os.path.getsize(path)
            start, end = 0, size - 1
            match = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
            ranged = bool(match and (match.group(1) or match.group(2)) and not args.no_ranges)
            if ranged:
                if match.group(1):
                    start = int(match.group(1))
                    if match.group(2):
                        end = min(int(match.group(2)), size - 1)
                else:
                    start = max(size - int(match.group(2)), 0)
            if args.refuse_after_seconds is not None:
                with state.lock:
                    now = time.monotonic()
                    if state.first_request is None:
                        state.first_request = now
                    refused = (now - state.first_request > args.refuse_after_seconds
                               and (path, start) not in state.range_ends)
                    if not refused:
                        state.range_ends.add((path, end + 1))
                if refused:
                    self.send_error(403)
                    return
            if ranged:
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

            with state.lock:
                state.responses += 1
                stalls = state.responses > args.stall_skip
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
                        if (args.stall_at is not None and stalls
                                and offset <= args.stall_at < offset + len(chunk)
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

        def serve_live(self, vod, head):
            vod = os.path.join(root, vod.lstrip("/"))
            target = 0
            segments = []  # (EXTINF line, URI)
            extinf = None
            header = []  # EXT-X-MAP
            with open(vod) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("#EXT-X-TARGETDURATION:"):
                        target = line.split(":", 1)[1]
                    elif line.startswith("#EXT-X-MAP:"):
                        header.append(line)
                    elif line.startswith("#EXTINF:"):
                        extinf = line
                    elif line and not line.startswith("#"):
                        segments.append((extinf, line))
            with state.lock:
                if state.live_start is None:
                    state.live_start = time.monotonic()
                elapsed = time.monotonic() - state.live_start
            # Segment S + k is out once segments S .. S + k - 1 have played.
            out = min(args.live_start, len(segments))
            played = 0.0
            while out < len(segments):
                played += float(segments[out][0][8:].split(",")[0])
                if played > elapsed:
                    break
                out += 1
            first = max(out - args.live_window, 0) if args.live_window else 0
            lines = ["#EXTM3U", "#EXT-X-VERSION:7", f"#EXT-X-TARGETDURATION:{target}",
                     f"#EXT-X-MEDIA-SEQUENCE:{first}"] + header
            if not args.live_window:
                lines.append("#EXT-X-PLAYLIST-TYPE:EVENT")
            for extinf, uri in segments[first:out]:
                lines += [extinf, uri]
            if out == len(segments):
                lines.append("#EXT-X-ENDLIST")
            body = ("\n".join(lines) + "\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.apple.mpegurl")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)

        def serve_live_flv(self, head):
            header, tags = read_flv(os.path.join(root, args.live_flv[1].lstrip("/")))
            with state.lock:
                now = time.monotonic()
                if state.flv_start is None:
                    state.flv_start = now
                state.flv_requests += 1
                first = state.flv_requests == 1
                start = state.flv_start
            # A tag at |ms| is due at this time.
            def due(ms):
                return start + ms / 1000 - args.live_flv_burst
            out = []
            if first:
                out = list(tags)
            else:
                # The header tags, then from the last keyframe that is due.
                configs = [t for t in tags if t["config"] or t["type"] == 18]
                begin = 0
                for i, t in enumerate(tags):
                    if due(t["ms"]) > now:
                        break
                    if t["key"]:
                        begin = i
                out = configs + [t for t in tags[begin:] if not t["config"] and t["type"] != 18]
            base = -args.live_flv_uptime
            if not first and args.live_flv_restart:
                base = min((t["ms"] for t in out if not t["config"] and t["type"] != 18), default=0)
            self.send_response(200)
            self.send_header("Content-Type", "video/x-flv")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            if head:
                return
            drop = (start + args.live_flv_drop_after
                    if args.live_flv_drop_after is not None and first else None)

            def send(data):
                self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
                self.wfile.flush()

            try:
                send(header)
                for t in out:
                    wait = due(t["ms"]) - time.monotonic()
                    if drop is not None and time.monotonic() + max(wait, 0) >= drop:
                        time.sleep(max(drop - time.monotonic(), 0))
                        # A reset, which clients report as an error (a
                        # close some take for the end of a chunked body).
                        # close() waits for rfile and wfile to let go of
                        # the socket; the descriptor closes now.
                        self.close_connection = True
                        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                   struct.pack("ii", 1, 0))
                        os.close(self.connection.detach())
                        return
                    if wait > 0:
                        time.sleep(wait)
                    send(retime(t, t["ms"] - base if t["type"] != 18 and not t["config"] else 0))
                self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError, OSError):
                return

    return Handler


def read_flv(path):
    # The header with the first previous-tag size, and each tag with its
    # previous-tag size after it: its type, time, whether it is a sequence
    # header or a video keyframe.
    data = open(path, "rb").read()
    offset = struct.unpack(">I", data[5:9])[0] + 4
    header = data[:offset]
    tags = []
    while offset + 15 <= len(data):
        kind = data[offset]
        size = int.from_bytes(data[offset + 1:offset + 4], "big")
        ms = int.from_bytes(data[offset + 4:offset + 7], "big") | data[offset + 7] << 24
        payload = data[offset + 11:offset + 11 + size]
        config = kind in (8, 9) and len(payload) > 1 and payload[1] == 0
        key = kind == 9 and len(payload) > 1 and payload[0] >> 4 == 1 and payload[1] == 1
        tags.append({"type": kind, "ms": ms, "config": config, "key": key,
                     "bytes": data[offset:offset + 15 + size]})
        offset += 15 + size
    return header, tags


def retime(tag, ms):
    out = bytearray(tag["bytes"])
    out[4:7] = (ms & 0xFFFFFF).to_bytes(3, "big")
    out[7] = (ms >> 24) & 0xFF
    return bytes(out)


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
    parser.add_argument("--stall-skip", type=int, default=0)
    parser.add_argument("--drop-at", type=int)
    parser.add_argument("--refuse-after-drop", action="store_true")
    parser.add_argument("--refuse-after-requests", type=int)
    parser.add_argument("--refuse-after-seconds", type=float)
    parser.add_argument("--no-ranges", action="store_true")
    parser.add_argument("--latency", type=float, default=0)
    parser.add_argument("--redirect", nargs=2, metavar=("FROM", "TO"))
    parser.add_argument("--status", nargs=2, action="append",
                        metavar=("PATH", "CODE"))
    parser.add_argument("--require-header", nargs=2, action="append",
                        metavar=("NAME", "VALUE"))
    parser.add_argument("--live", nargs=2, action="append", metavar=("PATH", "VOD"))
    parser.add_argument("--live-start", type=int, default=3)
    parser.add_argument("--live-window", type=int, default=4)
    parser.add_argument("--live-flv", nargs=2, metavar=("PATH", "FLV"))
    parser.add_argument("--live-flv-burst", type=float, default=0)
    parser.add_argument("--live-flv-drop-after", type=float)
    parser.add_argument("--live-flv-restart", action="store_true")
    parser.add_argument("--live-flv-uptime", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    server = Server(("127.0.0.1", args.port),
                    make_handler(args.root, args, State()))
    print(f"port {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
