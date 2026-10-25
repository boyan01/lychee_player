#!/usr/bin/env python3
# Serves a directory over HTTP with Range support, for network playback
# tests: Python's http.server ignores Range, and FFmpeg then parses the wrong
# bytes after every seek.
#
#   python3 tool/serve_media.py /tmp/lychee-media 8765 [HOST]
#
# The Android emulator reaches it at http://10.0.2.2:8765.
import http.server, os, re, sys

class Handler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        start, end = 0, size - 1
        match = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
        f = open(path, "rb")
        if match:
            if match.group(1):
                start = int(match.group(1))
                if match.group(2):
                    end = min(int(match.group(2)), size - 1)
            else:
                start = size - int(match.group(2))
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        f.seek(start)
        self.remaining = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        while self.remaining > 0:
            chunk = source.read(min(65536, self.remaining))
            if not chunk:
                break
            self.remaining -= len(chunk)
            outputfile.write(chunk)

    def log_message(self, *args):
        pass

os.chdir(sys.argv[1])
# Pass 0.0.0.0 as a third argument to reach it from other machines.
host = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
http.server.ThreadingHTTPServer((host, int(sys.argv[2])), Handler).serve_forever()
