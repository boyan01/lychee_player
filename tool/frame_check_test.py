#!/usr/bin/env python3
"""Independent colorimetric and sampling checks of frame_check's reference.

The fixture starts as known, in-gamut linear BT.709 light, changes basis to
BT.2020, applies ST 2084, and quantizes to limited-range 10-bit YUV. No
FFmpeg color conversion generates it. Since every color is inside BT.709,
relative-colorimetric mapping must preserve that light. This catches the
swscale RGB8 / full_chroma_int paths changing colored pixels.
The bilinear fixtures check pixel-center sampling and edge clamping
without using FFmpeg to generate the expected resized pixels.
"""

import importlib.util
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import unittest

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "frame_check", Path(__file__).with_name("frame_check.py"))
frame_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frame_check)


def pq(nits):
    value = (nits / 10000) ** (2610 / 16384)
    return ((3424 / 4096 + 2413 / 128 * value) /
            (1 + 2392 / 128 * value)) ** (2523 / 32)


class HdrReferenceTest(unittest.TestCase):
    def test_in_gamut_light_is_preserved(self):
        # Positive channels avoid a gamut-boundary decision. A single color
        # covers 32x32 pixels so interpolation cannot change its center.
        colors = [(20, 4, 5), (4, 20, 5), (4, 5, 20), (20, 20, 4),
                  (4, 20, 20), (20, 4, 20), (20, 20, 20), (4, 4, 4)]
        planes = [bytearray(), bytearray(), bytearray()]
        quantized_light = []
        for rgb in colors:
            bt2020 = [sum(a * b for a, b in zip(row, rgb)) for row in (
                (0.627404, 0.329283, 0.043313),
                (0.069097, 0.919540, 0.011362),
                (0.016391, 0.088013, 0.895595))]
            r, g, b = map(pq, bt2020)
            y = 0.2627 * r + 0.6780 * g + 0.0593 * b
            cb = (b - y) / (2 * (1 - 0.0593))
            cr = (r - y) / (2 * (1 - 0.2627))
            codes = [round(64 + 876 * y), round(512 + 896 * cb),
                     round(512 + 896 * cr)]
            for plane, value in zip(planes, codes):
                plane.extend(struct.pack("<H", value) * 32)
            # Account for source quantization before asserting preservation.
            y, cb, cr = ((codes[0] - 64) / 876, (codes[1] - 512) / 896,
                         (codes[2] - 512) / 896)
            r, b = y + 2 * (1 - 0.2627) * cr, y + 2 * (1 - 0.0593) * cb
            g = (y - 0.2627 * r - 0.0593 * b) / 0.6780
            light = []
            for channel in (r, g, b):
                value = channel ** (32 / 2523)
                light.append(10000 * (max(value - 3424 / 4096, 0) /
                                      (2413 / 128 - 2392 / 128 * value)) **
                             (16384 / 2610))
            quantized_light.append([sum(a * b for a, b in zip(row, light))
                                    for row in (
                                        (1.660491, -0.587641, -0.072850),
                                        (-0.124550, 1.132900, -0.008349),
                                        (-0.018151, -0.100579, 1.118730))])
        raw = b"".join(bytes(plane) * 32 for plane in planes)
        with tempfile.TemporaryDirectory(prefix="lychee-hdr-reference-") as tmp:
            source, media = Path(tmp) / "source.yuv", Path(tmp) / "colors.mkv"
            source.write_bytes(raw)
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-f", "rawvideo",
                "-pixel_format", "yuv444p10le", "-video_size", "256x32",
                "-framerate", "1", "-color_range", "tv", "-colorspace", "bt2020nc",
                "-color_primaries", "bt2020", "-color_trc", "smpte2084",
                "-i", str(source), "-frames:v", "1",
                "-c:v", "ffv1", "-color_range", "tv", "-colorspace", "bt2020nc",
                "-color_primaries", "bt2020", "-color_trc", "smpte2084",
                str(media)], check=True)
            images = [frame_check.decode(str(media), 0, width, height,
                                         "bt2020nc")[0]
                      for width, height in ((256, 32), (512, 64))]
        for shown in images:
            scale = shown.width // 256
            for index, rgb in enumerate(colors):
                expected = [round(255 * (channel / 203) ** (1 / 2.4))
                            for channel in quantized_light[index]]
                at = (16 * scale * shown.width +
                      (index * 32 + 16) * scale) * 3
                actual = list(shown.pixels[at:at + 3])
                with self.subTest(nits=rgb, size=(shown.width, shown.height)):
                    self.assertLessEqual(max(abs(a - b) for a, b in
                                             zip(actual, expected)), 3,
                                         f"{actual} versus {expected}")


class BilinearReferenceTest(unittest.TestCase):
    def test_downscale_does_not_average_the_whole_source_footprint(self):
        # Each output pixel covers six source pixels, but a texture's
        # bilinear sampler reads only the two around the pixel center.
        row = bytes(value for value in (0, 0, 240, 240, 0, 0,
                                       255, 255, 0, 0, 255, 255)
                    for _ in range(3))
        with tempfile.TemporaryDirectory(prefix="lychee-bilinear-reference-") as tmp:
            source = Path(tmp) / "pattern.ppm"
            frame_check.write_ppm(source, frame_check.Image(12, 4, row * 4))
            image, pts = frame_check.decode(str(source), 0, 2, 1, "bt709",
                                            bilinear=True)
        self.assertEqual(pts, 0)
        self.assertEqual(image.pixels, bytes((240, 240, 240, 0, 0, 0)))

    def test_upscale_clamps_to_the_source_edges(self):
        for width, height in ((2, 1), (1, 2)):
            with self.subTest(size=(width, height)):
                image = frame_check.Image(width, height,
                                           bytes((0, 0, 0, 255, 255, 255)))
                scaled = frame_check.resize_bilinear(image, width * 2,
                                                     height * 2)
                values = (0, 64, 191, 255)
                expected = (bytes(value for value in values for _ in range(3)) * 2
                            if width == 2 else
                            bytes(value for value in values for _ in range(6)))
                self.assertEqual(scaled.pixels, expected)


if __name__ == "__main__":
    unittest.main()
