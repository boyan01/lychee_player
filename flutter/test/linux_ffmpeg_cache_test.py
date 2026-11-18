"""Run with python3 -I flutter/test/linux_ffmpeg_cache_test.py."""

from functools import partial
from hashlib import sha256
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from threading import Event, Thread
import unittest


ASSET = "ffmpeg-linux-x86_64"
TAG = "v-test"
LIBRARIES = ["avformat", "avcodec", "swresample", "swscale", "avutil"]
FETCH = Path(__file__).resolve().parents[1] / "linux" / "ffmpeg.cmake"


class CacheTest(unittest.TestCase):
    def test_concurrent_configures_share_a_complete_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = root / ASSET
            (payload / "lib").mkdir(parents=True)
            (payload / "include").mkdir()
            (payload / "include" / "fixture.h").write_text("/* FFmpeg */\n")
            for library in LIBRARIES:
                so = payload / "lib" / f"lib{library}.so.1"
                so.write_text(f"lib{library} license: LGPL version 2.1 or later\n")

            (root / TAG).mkdir()
            archive = root / TAG / f"{ASSET}.tar.xz"
            with tarfile.open(archive, "w:xz") as tar:
                tar.add(payload, arcname=ASSET)
            digest = sha256(archive.read_bytes()).hexdigest()
            (root / "linux").mkdir()
            shutil.copyfile(FETCH, root / "linux" / "ffmpeg.cmake")
            (root / "CMakeLists.txt").write_text(
                """cmake_minimum_required(VERSION 3.20)
project(cache_test NONE)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
include(linux/ffmpeg.cmake)
lychee_fetch_ffmpeg("${CMAKE_SOURCE_DIR}/cache" ffmpeg)
file(READ "${ffmpeg}/.lychee-sha256" digest)
if(NOT digest STREQUAL expected_digest)
  message(FATAL_ERROR "Incomplete FFmpeg cache")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/root.txt" "${ffmpeg}")
configure_file("${ffmpeg}/include/fixture.h" fixture.h COPYONLY)
foreach(lib IN LISTS LYCHEE_FFMPEG_LIBRARIES)
  configure_file("${ffmpeg}/lib/lib${lib}.so.1" "lib${lib}.so.1" COPYONLY)
endforeach()
"""
            )
            download_started = Event()
            second_download = Event()
            requests = []

            class Handler(SimpleHTTPRequestHandler):
                def do_GET(self):
                    requests.append(self.path)
                    if download_started.is_set():
                        second_download.set()
                    else:
                        download_started.set()
                        # Keep the first download in flight while the second
                        # configure reaches the shared cache.
                        second_download.wait(timeout=2)
                    super().do_GET()

                def log_message(self, *_):
                    pass

            def configure(name):
                return subprocess.Popen(
                    ["cmake", "-S", root, "-B", root / name,
                     f"-Dexpected_digest={digest}"],
                    env={**os.environ, "LYCHEE_PREBUILT_URL": ""},
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                )

            def check_finished(process):
                try:
                    output, _ = process.communicate(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise
                self.assertEqual(process.returncode, 0, output)

            with ThreadingHTTPServer(
                ("127.0.0.1", 0), partial(Handler, directory=root)
            ) as server:
                (root / "prebuilt.json").write_text(json.dumps({
                    "url": f"http://127.0.0.1:{server.server_port}",
                    "tag": TAG,
                    "files": {archive.name: digest},
                }))
                thread = Thread(target=server.serve_forever)
                thread.start()
                try:
                    with configure("first") as first:
                        self.assertTrue(download_started.wait(timeout=10))
                        with configure("second") as second:
                            check_finished(second)
                        check_finished(first)
                finally:
                    server.shutdown()
                    thread.join()

            self.assertEqual(requests, [f"/{TAG}/{archive.name}"])
            # With the mirror gone, the warm cache must still resolve.
            with configure("warm") as warm:
                check_finished(warm)
            cached = (root / "first" / "root.txt").read_text()
            for name in ("first", "second", "warm"):
                build = root / name
                self.assertEqual((build / "root.txt").read_text(), cached)
                self.assertEqual((build / "fixture.h").read_text(), "/* FFmpeg */\n")
                for library in LIBRARIES:
                    so = Path("lib") / f"lib{library}.so.1"
                    self.assertEqual((build / so.name).read_bytes(),
                                     (payload / so).read_bytes())


if __name__ == "__main__":
    unittest.main()
