#!/usr/bin/env python3
"""Check native video composition and frame delivery in the Flutter example.

Install a profile example built with LYCHEE_SURFACE_VIEW=true first. The
URL must be playable under the app's network security configuration. This
check needs a real device capable of the requested video frame rate; it
does not install an APK or change device settings.
"""

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import time


def video_composition(dump, package, size):
    """Require the actual video layer, not Flutter's final window."""
    matches = []
    for section in dump.split("- Output Layer ")[1:]:
        title = section.splitlines()[0]
        if package not in title or "SurfaceView[" not in title:
            continue
        crop = re.search(r"sourceCrop=\[([0-9. ]+)\]", section)
        composition = re.search(r"hwc:.*composition=(\w+)", section)
        if not crop or not composition:
            raise ValueError("unsupported video layer dump format")
        left, top, right, bottom = map(float, crop[1].split())
        if (right - left, bottom - top) == size:
            matches.append(composition[1])
    if len(matches) != 1:
        raise ValueError("expected exactly one visible native video layer of the requested size")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--size", default="3840x2160")
    parser.add_argument("--min-fps", type=float, default=54)
    parser.add_argument("--max-drop-ratio", type=float, default=0.05)
    parser.add_argument("--seconds", type=int, default=12)
    parser.add_argument("--package", default="tech.soit.lychee.lychee_player_example")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.size):
        parser.error("--size must be WIDTHxHEIGHT")
    if args.seconds < 8 or args.min_fps <= 0 or not 0 <= args.max_drop_ratio <= 1:
        parser.error("invalid duration or frame-delivery threshold")
    size = tuple(map(int, args.size.split("x")))
    args.out.mkdir(parents=True, exist_ok=True)

    def adb(*parts):
        if parts and parts[0] == "shell":
            parts = ("shell", shlex.join(parts[1:]))
        return subprocess.check_output(["adb", "-s", args.device, *parts], text=True)

    # FlutterActivity accepts a serialized string list for main(List<String>).
    dart_args = ",".join(value.replace(",", r"\,") for value in
                         ["--seconds", str(args.seconds), args.url])
    compositions = []
    try:
        adb("shell", "am", "start", "-W", "-S", "-n", args.package + "/.MainActivity",
            "--esal", "dart_entrypoint_args", dart_args,
            "--es", "url", args.url)
        started = time.monotonic()
        pid = adb("shell", "pidof", args.package).strip()
        for index, fraction in enumerate((0.25, 0.5, 0.75)):
            time.sleep(max(0, started + args.seconds * fraction - time.monotonic()))
            dump = adb("shell", "dumpsys", "SurfaceFlinger")
            (args.out / f"surfaceflinger-{index}.txt").write_text(dump)
            compositions.append(video_composition(dump, args.package, size))
        time.sleep(max(0, started + args.seconds + 3 - time.monotonic()))
        log = adb("logcat", "-d", "--pid=" + pid)
        summaries = re.findall(r"summary:.*", log)
        if not summaries:
            raise ValueError("the example did not report a playback summary")
        summary = summaries[-1]
        (args.out / "summary.txt").write_text(summary + "\n")
        counts = re.search(r"rendered=(\d+) dropped=(\d+) position=([0-9.]+)", summary)
        if not counts or float(counts[3]) <= 0:
            raise ValueError("the example did not play video")
        rendered, dropped, position = int(counts[1]), int(counts[2]), float(counts[3])
        result = {
            "composition": compositions,
            "rendered": rendered,
            "dropped": dropped,
            "position": position,
            "acquired_per_second": rendered / position,
            "drop_ratio": dropped / max(rendered + dropped, 1),
        }
        result["pass"] = (all(value == "DEVICE" for value in compositions)
                          and result["acquired_per_second"] >= args.min_fps
                          and result["drop_ratio"] <= args.max_drop_ratio)
        (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(summary)
        print(json.dumps(result, indent=2))
        return 0 if result["pass"] else 1
    finally:
        adb("shell", "am", "force-stop", args.package)


if __name__ == "__main__":
    raise SystemExit(main())
