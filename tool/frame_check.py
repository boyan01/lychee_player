#!/usr/bin/env python3
"""Checks a rendered frame against the media it came from, by pixels.

lychee_cli --frame-ppm and examples/glfw --snapshot write what they show as
binary PPM and print a line for it:

  snapshot FILE pts=US size=WxH [video=X,Y,W,H]

This decodes the frame at that pts with the ffmpeg command-line tool,
upright and scaled to the video rectangle (the whole picture without one),
and compares the two. It prints PASS or FAIL with the numbers, and on FAIL
writes FILE.diff.ppm (the absolute difference, four times brighter).

  frame_check.py MEDIA --log OUTPUT         every snapshot line in OUTPUT
                                            (- reads standard input)
  frame_check.py MEDIA FILE --pts US [--video X,Y,W,H]
  frame_check.py MEDIA ... --matrix bt601    the reference in another
                                            YCbCr matrix
  frame_check.py MEDIA ... --bilinear        native RGB, then a texture's
                                            bilinear sampling at pixel centers
  frame_check.py MEDIA FILE --pts US [--video X,Y,W,H] --self-test
                                            also checks that wrong frames
                                            made from FILE fail (written
                                            next to it)

The reference follows the library's color conventions: an untagged matrix
is BT.709 above 576 lines and BT.601 otherwise, and HDR is tone-mapped to
SDR BT.709 by swscale, as the core's software path does.

Three numbers decide, each against a threshold calibrated on the
regression media (tool/regression.sh): PSNR and mean absolute error over
the picture catch a wrong picture as a whole (black, swapped color
matrix, wrong scale), and the mean absolute error of the worst 32x32
tile catches small differences in a small area: the frame before or
after shows the same test pattern with a moving part elsewhere. Outside
the video rectangle a window must be black.

Python standard library only; run it as python3 -I tool/frame_check.py.
"""

import argparse
import array
import functools
import json
import math
import os
import re
import subprocess
import sys

# Calibrated on tool/regression.sh's media through lychee_cli (--cpu,
# --native, --no-hw) and examples/glfw; docs/development.md has the numbers.
MIN_PSNR = 28.0
MAX_MAE = 4.0
MAX_TILE_MAE = 40.0
# Mean of the window outside the video rectangle.
MAX_BORDER_MEAN = 2.0
TILE = 32

SNAPSHOT_LINE = re.compile(
    r"snapshot (?P<path>.+?) pts=(?P<pts>-?\d+) size=(?P<w>\d+)x(?P<h>\d+)"
    r"(?: video=(?P<video>-?\d+,-?\d+,\d+,\d+))?\s*$")


class Image:
    """RGB, 3 bytes a pixel, top row first."""

    def __init__(self, width, height, pixels):
        assert len(pixels) == width * height * 3
        self.width = width
        self.height = height
        self.pixels = pixels

    def crop(self, x, y, w, h):
        rows = []
        for row in range(y, y + h):
            start = (row * self.width + x) * 3
            rows.append(self.pixels[start:start + w * 3])
        return Image(w, h, b"".join(rows))


def read_ppm(path):
    with open(path, "rb") as file:
        data = file.read()
    # Header: P6, width, height, maxval, each after whitespace; comments
    # do not occur in what we write.
    fields = []
    at = 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    at += 1
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit binary PPM")
    width, height = int(fields[1]), int(fields[2])
    return Image(width, height, data[at:at + width * height * 3])


def write_ppm(path, image):
    with open(path, "wb") as file:
        file.write(b"P6\n%d %d\n255\n" % (image.width, image.height))
        file.write(image.pixels)


def resize_bilinear(image, width, height):
    """A clamp-to-edge texture sampler, without a downscaling low-pass.

    swscale's bilinear filter widens its kernel when downscaling. GPU
    textures without mipmaps instead read only the four source pixels
    around each output pixel center, even for a large reduction.
    """
    if (width, height) == (image.width, image.height):
        return image

    def axis(source, target):
        samples = []
        for at in range(target):
            position = max(0, min(source - 1,
                                  (at + 0.5) * source / target - 0.5))
            first = int(position)
            samples.append((first, min(first + 1, source - 1), position - first))
        return samples

    columns = axis(image.width, width)
    pixels = image.pixels
    result = bytearray()
    for top, bottom, fy in axis(image.height, height):
        for left, right, fx in columns:
            tl, tr = ((top * image.width + left) * 3,
                      (top * image.width + right) * 3)
            bl, br = ((bottom * image.width + left) * 3,
                      (bottom * image.width + right) * 3)
            for channel in range(3):
                upper = (pixels[tl + channel] * (1 - fx) +
                         pixels[tr + channel] * fx)
                lower = (pixels[bl + channel] * (1 - fx) +
                         pixels[br + channel] * fx)
                result.append(round(upper * (1 - fy) + lower * fy))
    return Image(width, height, bytes(result))


@functools.lru_cache
def probe(media):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,color_space,color_transfer,r_frame_rate:"
         "stream_side_data=rotation", "-of", "json", media],
        check=True, capture_output=True, text=True).stdout
    stream = json.loads(out)["streams"][0]
    rotation = 0
    for side_data in stream.get("side_data_list", []):
        rotation = int(side_data.get("rotation", 0))
    num, den = stream.get("r_frame_rate", "30/1").split("/")
    return {
        "width": stream["width"],
        "height": stream["height"],
        "matrix": stream.get("color_space", "unknown"),
        "transfer": stream.get("color_transfer", "unknown"),
        "quarter_turn": rotation % 180 != 0,
        "frame_us": round(1e6 * int(den) / int(num)),
    }


def same_aspect(width, height, upright):
    """Whether width x height is |upright| scaled, give or take the two
    pixels aspect-fit rounding can cost."""
    return abs(width * upright[1] - height * upright[0]) <= \
        2 * (upright[0] + upright[1])


def library_matrix(info):
    """The YCbCr matrix the library assumes, by ffmpeg's name."""
    if info["matrix"] not in ("unknown", "", None):
        return info["matrix"]
    return "bt709" if info["height"] > 576 else "bt601"


def decode(media, pts, width, height, matrix, bilinear=False):
    """The frame at |pts| (microseconds, the stream's own timeline),
    upright and scaled to width x height, and its pts as decoded."""
    info = probe(media)
    hdr = info["transfer"] in ("smpte2084", "arib-std-b67")
    output_width, output_height = width, height
    if bilinear:
        width, height = ((info["height"], info["width"]) if info["quarter_turn"]
                         else (info["width"], info["height"]))
    # full_chroma_int selects a different swscale HDR conversion path that
    # changes flat colors even at 1:1. Leave SDR's established flags alone.
    flags = "bilinear+accurate_rnd" + ("" if hdr else "+full_chroma_int")
    scale = (f"scale={width}:{height}:flags={flags}:"
             f"in_color_matrix={matrix}")
    if hdr:
        # Keep color conversion at native size. Fusing it with a resize
        # selects swscale paths that change even the center of flat colors.
        # Resize only the resulting SDR RGB, as a texture consumer does.
        scale = ("scale=iw:ih:flags=bilinear+accurate_rnd:"
                 f"in_color_matrix={matrix}:out_primaries=bt709:out_transfer=bt709,"
                 "format=rgb48le,"
                 f"scale={width}:{height}:flags=bilinear+accurate_rnd")
    # swscale's direct RGB8 output changes chromatic HDR colors. Retain
    # precision through tone/gamut mapping, then quantize the SDR result,
    # just as the core's FFmpeg CPU converter does. SDR stays unchanged.
    pixel_format = "rgb48le" if hdr else "rgb24"
    # -copyts keeps the stream's timestamps, which the library reports;
    # decoding from the start finds the frame in any container.
    graph = f"select='gte(t\\,{(pts - 500) / 1e6:.6f})',showinfo,{scale}"
    run = subprocess.run(
        ["ffmpeg", "-nostdin", "-hide_banner", "-v", "info", "-copyts", "-i",
         media, "-map", "0:v:0", "-vf", graph, "-frames:v", "1", "-f",
         "rawvideo", "-pix_fmt", pixel_format, "pipe:1"],
        capture_output=True)
    found = re.search(rb"pts_time:(-?[0-9.]+)", run.stderr)
    if run.returncode != 0 or not found or \
            len(run.stdout) != width * height * (6 if hdr else 3):
        tail = run.stderr.decode(errors="replace").strip().splitlines()[-3:]
        raise RuntimeError(f"ffmpeg found no frame at {pts} us: "
                           + " | ".join(tail))
    pixels = run.stdout
    if hdr:
        words = array.array("H", pixels)
        if sys.byteorder != "little":
            words.byteswap()
        pixels = bytes((value + 128) // 257 for value in words)
    reference = Image(width, height, pixels)
    if bilinear:
        reference = resize_bilinear(reference, output_width, output_height)
    return reference, round(float(found[1]) * 1e6)


def compare(shown, reference):
    """PSNR, mean absolute error, the worst tile's mean absolute error
    with its position, and the absolute difference."""
    diff = bytes(map(abs, map(int.__sub__, shown.pixels, reference.pixels)))
    histogram = [0] * 256
    for value in diff:
        histogram[value] += 1
    count = len(diff)
    squared = sum(value * value * n for value, n in enumerate(histogram))
    mse = squared / count
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    mae = sum(value * n for value, n in enumerate(histogram)) / count
    worst, worst_at = 0.0, (0, 0)
    width, height = shown.width, shown.height
    for ty in range(0, height, TILE):
        rows = range(ty, min(ty + TILE, height))
        for tx in range(0, width, TILE):
            tw = min(TILE, width - tx)
            total = 0
            for row in rows:
                start = (row * width + tx) * 3
                total += sum(diff[start:start + tw * 3])
            tile = total / (len(rows) * tw * 3)
            if tile > worst:
                worst, worst_at = tile, (tx, ty)
    return psnr, mae, worst, worst_at, diff


def evaluate(media, path, pts, video, matrix=None, bilinear=False):
    """Compares the snapshot at |path| with the frame at |pts|: the
    problems found (none when it passes), the numbers, and the absolute
    difference with its size, or None before a comparison."""
    shown = read_ppm(path)
    info = probe(media)
    upright = (info["height"], info["width"]) if info["quarter_turn"] \
        else (info["width"], info["height"])
    if video is None:
        x, y, w, h = 0, 0, shown.width, shown.height
        if (w, h) != upright:
            return [f"size {w}x{h}, the video is {upright[0]}x{upright[1]}"
                    ], "", None
    else:
        x, y, w, h = video
        if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > shown.width or \
                y + h > shown.height:
            return [f"video {x},{y},{w},{h} outside the "
                    f"{shown.width}x{shown.height} window"], "", None
        if not same_aspect(w, h, upright):
            return [f"video {w}x{h} is not the aspect of "
                    f"{upright[0]}x{upright[1]}"], "", None

    problems = []
    reference, ref_pts = decode(media, pts, w, h,
                                matrix or library_matrix(info), bilinear)
    if abs(ref_pts - pts) > 1000:
        problems.append(f"no frame at pts {pts} (next is {ref_pts})")
    region = shown if video is None else shown.crop(x, y, w, h)
    psnr, mae, tile, tile_at, diff = compare(region, reference)
    numbers = (f"psnr={psnr:.1f}dB mae={mae:.2f} "
               f"tile_mae={tile:.1f}@{tile_at[0] + x},{tile_at[1] + y}")
    if psnr < MIN_PSNR:
        problems.append(f"psnr<{MIN_PSNR:g}")
    if mae > MAX_MAE:
        problems.append(f"mae>{MAX_MAE:g}")
    if tile > MAX_TILE_MAE:
        problems.append(f"tile_mae>{MAX_TILE_MAE:g}")
    if video is not None:
        border = 0.0
        if (w, h) != (shown.width, shown.height):
            outside = sum(shown.pixels) - sum(region.pixels)
            border = outside / ((shown.width * shown.height - w * h) * 3)
        numbers += f" border={border:.1f}"
        if border > MAX_BORDER_MEAN:
            problems.append(f"border>{MAX_BORDER_MEAN:g}")
    return problems, numbers, Image(w, h, diff)


def check(media, path, pts, video, matrix=None, bilinear=False):
    """Prints PASS or FAIL for the snapshot at |path|, writing the
    difference next to it on FAIL. Returns whether it passed."""
    name = os.path.basename(path)
    try:
        problems, numbers, diff = evaluate(media, path, pts, video, matrix,
                                           bilinear)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL {name}: {error}")
        return False
    if not problems:
        print(f"PASS {name} pts={pts} {numbers}")
        return True
    if diff is None:
        print(f"FAIL {name}: " + "; ".join(problems))
        return False
    out = os.path.splitext(path)[0] + ".diff.ppm"
    diff.pixels = bytes(min(255, 4 * v) for v in diff.pixels)
    write_ppm(out, diff)
    print(f"FAIL {name} pts={pts} {numbers}: " + ", ".join(problems)
          + f" (diff: {out})")
    return False


def ffmpeg_filter(path, out, graph):
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", path,
                    "-vf", graph, "-pix_fmt", "rgb24", out], check=True)


def self_test(media, path, pts, video, matrix=None, bilinear=False):
    """The snapshot must pass, and each wrong frame made from it fail."""
    if not check(media, path, pts, video, matrix, bilinear):
        print(f"FAIL self-test {os.path.basename(path)}: the snapshot itself")
        return False
    info = probe(media)
    shown = read_ppm(path)
    stem = os.path.splitext(path)[0]
    x, y, w, h = video or (0, 0, shown.width, shown.height)

    def variant(name, graph):
        out = f"{stem}.{name}.ppm"
        ffmpeg_filter(path, out, graph)
        return out

    def black():
        out = f"{stem}.black.ppm"
        write_ppm(out, Image(shown.width, shown.height,
                             bytes(len(shown.pixels))))
        return out

    # The video's picture changed in place, the rest of the window kept.
    def inside(graph):
        return (f"split[a][b];[b]crop={w}:{h}:{x}:{y},{graph}[v];"
                f"[a][v]overlay={x}:{y}")

    matrix = matrix or library_matrix(info)
    bt601 = matrix in ("bt601", "smpte170m", "bt470bg")
    other = "bt709" if bt601 else "bt601"
    cases = [
        ("frame before", path, pts - info["frame_us"], video, None),
        ("frame after", path, pts + info["frame_us"], video, None),
        ("turned 180", variant("turned", inside("hflip,vflip")), pts, video,
         None),
        ("turned 90", variant("transposed", "transpose=clock"), pts, None,
         None) if video is None else
        ("stretched", path, pts, (x, y, w, h * 3 // 4), None),
        ("2% larger", variant("larger", inside(
            f"crop=iw*0.98:ih*0.98,scale={w}:{h}")), pts, video, None),
        ("smaller", variant("smaller", f"scale={shown.width - 16}:"
                            f"{shown.height}"), pts, None, None)
        if video is None else
        ("4px to the left", variant(
            "shifted", "crop=iw-4:ih:4:0,pad=iw+4:ih:0:0"), pts, video, None),
        ("black", black(), pts, video, None),
        (f"read as {other}", path, pts, video, other),
    ]
    ok = True
    for name, case_path, case_pts, case_video, case_matrix in cases:
        problems, numbers, _ = evaluate(media, case_path, case_pts,
                                        case_video, case_matrix or matrix,
                                        bilinear)
        ok = ok and bool(problems)
        print(f"  {name}: " + (f"fails {numbers or problems[0]}" if problems
                               else f"PASSES (should fail) {numbers}"))
    print(("PASS" if ok else "FAIL") + f" self-test {os.path.basename(path)}")
    return ok


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("media")
    parser.add_argument("snapshot", nargs="?")
    parser.add_argument("--pts", type=int, help="microseconds")
    parser.add_argument("--video", help="X,Y,W,H in the snapshot")
    parser.add_argument("--log", help="CLI or harness output, - for stdin")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--bilinear", action="store_true",
        help="convert at native size, then sample like a GPU bilinear\n"
             "texture without mipmaps (no downscaling low-pass filter)")
    parser.add_argument(
        "--matrix",
        help="YCbCr matrix of the reference instead of the library's (an\n"
             "ffmpeg name, e.g. bt601): for a compositor that ignores the\n"
             "frame's, as the Android emulator's does")
    args = parser.parse_args()

    jobs = []
    ok = True
    if args.log:
        lines = sys.stdin if args.log == "-" else open(args.log)
        for line in lines:
            found = SNAPSHOT_LINE.search(line)
            if not found and line.startswith("snapshot "):
                # "snapshot FILE failed: ..."
                print("FAIL " + line.strip())
                ok = False
            elif found:
                video = found["video"]
                jobs.append((found["path"], int(found["pts"]),
                             tuple(map(int, video.split(","))) if video
                             else None))
        if not jobs and ok:
            print("FAIL no snapshot lines")
            return 1
    elif args.snapshot and args.pts is not None:
        jobs.append((args.snapshot, args.pts,
                     tuple(map(int, args.video.split(",")))
                     if args.video else None))
    else:
        parser.error("give SNAPSHOT --pts US, or --log OUTPUT")

    for path, pts, video in jobs:
        if args.self_test:
            ok = self_test(args.media, path, pts, video, args.matrix,
                           args.bilinear) and ok
        else:
            ok = check(args.media, path, pts, video, args.matrix,
                       args.bilinear) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
