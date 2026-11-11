#!/usr/bin/env python3
"""Check that the live FLV fixture really resets its TCP connection."""

import http.client
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class MediaServerTest(unittest.TestCase):
    def test_live_flv_drop_resets_the_connection(self):
        with tempfile.TemporaryDirectory() as directory:
            # Only the server reads these tags; no codec or media generator
            # is needed to check its transport behavior.
            header = b"FLV\x01\x01\x00\x00\x00\x09" + bytes(4)

            def tag(ms):
                payload = b"\x17\x01"
                body = (b"\x09" + len(payload).to_bytes(3, "big")
                        + ms.to_bytes(3, "big") + bytes(4) + payload)
                return body + len(body).to_bytes(4, "big")

            Path(directory, "live.flv").write_bytes(header + tag(0) + tag(5000))
            server = subprocess.Popen(
                [sys.executable, str(Path(__file__).with_name("media_server.py")),
                 directory, "--port", "0", "--live-flv", "/live.flv", "live.flv",
                 "--live-flv-drop-after", "0.05"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                port = int(server.stdout.readline().split()[1])
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                try:
                    connection.request("GET", "/live.flv")
                    response = connection.getresponse()
                    self.assertEqual(response.read(len(header)), header)
                    # A stalled socket times out instead. A complete chunked
                    # response would look like the stream's normal end.
                    with self.assertRaises((ConnectionError, http.client.IncompleteRead)):
                        response.read()
                finally:
                    connection.close()
            finally:
                server.terminate()
                server.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
