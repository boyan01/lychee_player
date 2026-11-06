#!/usr/bin/env python3
"""Check the installed native example's actual Android compositor output.

Requires an emulator, the example APK containing snapshot_pause, adb and
FFmpeg. No input events or screen-derived measurements are used. The
example reports its video bounds and held PTS; SurfaceFlinger supplies the
allocated buffer, source crop and dataspace. Original evidence is retained.
"""

import argparse
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import struct
import sys
import time
from urllib.parse import urlsplit

PACKAGE = "tech.soit.lychee.example"


def compositor_metadata(dump, video):
    """Fail closed when the live layer or buffer geometry is ambiguous."""
    selected = []
    for section in dump.split("- Output Layer ")[1:]:
        title = section.splitlines()[0]
        if "SurfaceView[" not in title or PACKAGE not in title:
            continue
        geometry = re.search(r"displayFrame=\[([0-9 ]+)\] sourceCrop=\[([0-9. ]+)\]"
                             r" bufferTransform=0 \(0\) dataspace=(\w+)", section)
        token = re.search(r"\((\w+) SurfaceView\[", title)
        if not geometry or not token:
            raise ValueError("unsupported SurfaceFlinger layer transform or dump format")
        frame = tuple(map(int, geometry[1].split()))
        bounds = (frame[0], frame[1], frame[2] - frame[0], frame[3] - frame[1])
        if bounds != tuple(video):
            continue
        crop = list(map(float, geometry[2].split()))
        if any(value != int(value) for value in crop):
            raise ValueError("fractional SurfaceFlinger crops are unsupported")
        left, top, right, bottom = map(int, crop)
        buffers = set()
        for line in dump.splitlines():
            if line.startswith("+ name:" + token[1] + " SurfaceView["):
                found = re.search(r"w/h:(\d+)x(\d+),.* req fmt:(\d+),", line)
                if found:
                    # YV12, flexible YUV420, or implementation-defined YUV.
                    if int(found[3]) not in (842094169, 35, 34):
                        raise ValueError("unsupported compositor buffer format")
                    buffers.add((int(found[1]), int(found[2])))
        if len(buffers) != 1:
            raise ValueError("live layer has missing or ambiguous imported buffer sizes")
        selected.append({"pixel_format": "yuv420", "buffer_size": list(buffers.pop()),
                         "crop": [left, top, right - left, bottom - top],
                         "dataspace": geometry[3]})
    if len(selected) != 1:
        raise ValueError("could not identify exactly one live video compositor layer")
    return selected[0]


def png_is_srgb(data):
    # AOSP screencap.cpp (capturedDataspace -> AndroidBitmap_compress):
    # https://android.googlesource.com/platform/frameworks/base/+/1cdfff555f4a21f71ccc978290e2e212e2f8b168/cmds/screencap/screencap.cpp
    # screencap passes capturedDataspace into AndroidBitmap_compress. Its
    # PNG sRGB chunk is direct evidence of the capture's output space,
    # rather than an assumption based on the display's advertised modes.
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return False
    offset = 8
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        if size > len(data) - offset - 12:
            return False
        kind = data[offset + 4:offset + 8]
        if kind == b"sRGB":
            return size == 1
        if kind == b"IDAT":
            return False
        offset += size + 12
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--media", required=True, help="same media file on the host")
    parser.add_argument("--seek", default="4.4,5.48", help="two playback positions in seconds")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.device.startswith("emulator-"):
        parser.error("this unattended harness requires an explicit emulator serial")
    targets = args.seek.split(",")
    if len(targets) != 2 or any(not math.isfinite(float(value)) or float(value) < 0 for value in targets):
        parser.error("--seek needs two nonnegative playback positions")
    args.out.mkdir(parents=True, exist_ok=True)

    def adb(*parts):
        if parts and parts[0] == "shell":
            parts = ("shell", shlex.join(parts[1:]))
        return subprocess.check_output(["adb", "-s", args.device, *parts])

    parsed = urlsplit(args.url)
    added_reverse = None
    if parsed.hostname in ("127.0.0.1", "localhost"):
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        endpoint = f"tcp:{port}"
        mappings = [line.split()[-2:] for line in adb("reverse", "--list").decode().splitlines() if line.strip()]
        if any(source == endpoint and target != endpoint for source, target in mappings):
            raise ValueError(f"{endpoint} already has a different reverse mapping")
        if [endpoint, endpoint] not in mappings:
            adb("reverse", endpoint, endpoint)
            added_reverse = endpoint
    try:
        adb("shell", "am", "start", "-S", "-n", PACKAGE + "/.MainActivity",
            "--es", "url", args.url, "--ez", "hw", "false", "--es", "seconds", "18",
            "--ez", "snapshot_pause", "true", "--es", "seek", f"5:{targets[0]},8:{targets[1]}",
            "--es", "snapshot", "3:compositor-prime.ppm,6:compositor-0.ppm,9:compositor-1.ppm")
        started = time.monotonic() + 5
        pid = ""
        while not pid and time.monotonic() < started:
            query = subprocess.run(["adb", "-s", args.device, "shell", "pidof", PACKAGE],
                                   capture_output=True, text=True)
            pid = query.stdout.strip()
            if not pid:
                time.sleep(0.1)
        if not pid:
            raise RuntimeError("the installed native example did not start")
        deadline = time.monotonic() + 40
        captured = set()
        passed = True
        while len(captured) != 2:
            if time.monotonic() > deadline:
                raise RuntimeError("timed out waiting for held compositor captures")
            log = adb("logcat", "-d", "-v", "brief", "-s", "lychee-example:I").decode()
            lines = [line for line in log.splitlines() if re.search(r"\(\s*" + pid + r"\)", line)]
            (args.out / "example.log").write_text("\n".join(lines) + "\n")
            pending = None
            for line in lines:
                found = re.search(r"snapshot .*compositor-([01])\.ppm pts=(\d+)", line)
                if found:
                    pending = (int(found[1]), int(found[2]))
                found = re.search(r"compositor pts=(\d+) video=(\d+,\d+,\d+,\d+)", line)
                if not found or pending is None or int(found[1]) != pending[1]:
                    continue
                index, pts = pending
                pending = None
                if index in captured:
                    continue
                video = tuple(map(int, found[2].split(",")))
                stem = args.out / f"frame-{index}"
                dump = adb("shell", "dumpsys", "SurfaceFlinger").decode()
                metadata = compositor_metadata(dump, video)
                stem.with_suffix(".surfaceflinger.txt").write_text(dump)
                png = adb("exec-out", "screencap", "-p")
                if not png_is_srgb(png):
                    raise ValueError("compositor PNG must explicitly declare sRGB output")
                metadata["output_dataspace"] = "V0_SRGB"
                stem.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")
                stem.with_suffix(".png").write_bytes(png)
                x, y, width, height = video
                subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(stem.with_suffix(".png")),
                                "-vf", f"crop={width}:{height}:{x}:{y}", "-y",
                                str(stem.with_suffix(".ppm"))], check=True)
                command = [sys.executable, "-I", str(Path(__file__).with_name("frame_check.py")),
                           args.media, str(stem.with_suffix(".ppm")), "--pts", str(pts),
                           "--video", f"0,0,{width},{height}", "--android-compositor",
                           str(stem.with_suffix(".json"))]
                result = subprocess.run(command, capture_output=True, text=True)
                print(result.stdout, end="", flush=True)
                stem.with_suffix(".check.log").write_text(result.stdout + result.stderr)
                stem.with_suffix(".capture.json").write_text(json.dumps(
                    {"pts": pts, "screen_video": video, "device": args.device,
                     "command": command}, indent=2) + "\n")
                passed = result.returncode == 0 and passed
                captured.add(index)
            if len(captured) != 2:
                time.sleep(0.2)
        return 0 if passed else 1
    finally:
        subprocess.run(["adb", "-s", args.device, "shell", "am", "force-stop", PACKAGE],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if added_reverse is not None:
            subprocess.run(["adb", "-s", args.device, "reverse", "--remove", added_reverse],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    sys.exit(main())
