#!/usr/bin/env python3
"""Independent analytic checks of Android compositor geometry and transfer.

The ramp and transfer landmarks are analytic values, not fitted captures.
SurfaceFlinger parsing tests ensure source geometry comes from the live
producer, and reject ambiguity instead of trying multiple references.
"""
import importlib.util
from pathlib import Path
import sys
import struct
import unittest

sys.dont_write_bytecode = True


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = load("frame_check")
capture = load("android_compositor_check")


def metadata(buffer=(10, 4), crop=(0, 0, 8, 4)):
    return {"pixel_format": "yuv420", "buffer_size": list(buffer), "crop": list(crop),
            "dataspace": "V0_BT601_525", "output_dataspace": "V0_SRGB"}


class AndroidCompositorTest(unittest.TestCase):
    def test_crop_only_applies_to_padded_axis(self):
        self.assertEqual(check.android_compositor_geometry(metadata(), 8, 4), (1, 0))
        self.assertEqual(check.android_compositor_geometry(metadata((8, 6)), 8, 4), (0, 1))
        self.assertEqual(check.android_compositor_geometry(metadata((8, 4)), 8, 4), (0, 0))
        self.assertEqual(check.android_compositor_geometry(metadata((10, 6), (2, 2, 8, 4)), 8, 4), (1, 1))

    def test_inset_uses_pixel_centers_without_changing_full_axis(self):
        # f(x,y) = 16*x + 4*y. Sampling the 1-texel-inset X axis into
        # four columns gives x=1.25,2.75,4.25,5.75; uninset Y gives .5,2.5.
        image = check.Image(8, 4, bytes(value for y in range(4) for x in range(8)
                                        for value in (16*x + 4*y,) * 3))
        sampled = check.resize_bilinear(image, 4, 2, (1, 0))
        self.assertEqual(list(sampled.pixels[::3]), [22, 46, 70, 94, 30, 54, 78, 102])
        # Padding is producer metadata, not something inferred from pixels.
        normal = check.resize_bilinear(image, 4, 2)
        self.assertNotEqual(sampled.pixels, normal.pixels)

    def test_transfer_landmarks_and_gray_neutrality(self):
        expected = {0: 0, 16: 31, 20: 36, 32: 48, 64: 79, 128: 140, 192: 198, 255: 255}
        for source, srgb in expected.items():
            with self.subTest(source=source):
                self.assertEqual(check.bt709_to_srgb(source), srgb)
        source = check.Image(8, 4, bytes([128, 128, 128] * 32))
        shown = check.android_compositor_reference(source, 16, 8, metadata())
        self.assertEqual(shown.pixels, bytes([140, 140, 140] * 128))

    def test_metadata_errors_are_not_silently_guessed(self):
        invalid = [metadata((7, 4)), metadata((10, 4), (3, 0, 8, 4)),
                   metadata((10, 4), (0, 0, 7, 4))]
        for key, value in (("pixel_format", "rgb"), ("dataspace", "UNKNOWN"),
                           ("output_dataspace", "DISPLAY_P3")):
            bad = metadata(); bad[key] = value; invalid.append(bad)
        for value in invalid:
            with self.subTest(metadata=value), self.assertRaises(ValueError):
                check.android_compositor_geometry(value, 8, 4)

    def test_output_space_comes_from_png_not_display_guess(self):
        png = b"\x89PNG\r\n\x1a\n"
        srgb = struct.pack(">I", 1) + b"sRGB\0" + bytes(4)
        self.assertTrue(capture.png_is_srgb(png + srgb))
        self.assertFalse(capture.png_is_srgb(png))
        self.assertFalse(capture.png_is_srgb(png + srgb[:-1]))
        self.assertFalse(capture.png_is_srgb(b"not a PNG"))

    def test_source_dump_supplies_geometry_not_screenshot(self):
        dump = """- Output Layer 0x1(abcd SurfaceView[tech.soit.lychee.example/MainActivity](BLAST)#4)
 displayFrame=[10 20 90 60] sourceCrop=[0.000000 0.000000 8.000000 4.000000] bufferTransform=0 (0) dataspace=V0_BT601_525 (281280512)
+ name:abcd SurfaceView[tech.soit.lychee.example/MainActivity]#3, id:7, w/h:10x4, req fmt:842094169,
- outputDataspace: (deprecated) sRGB sRGB Full range
"""
        expected = metadata()
        expected.pop("output_dataspace")
        self.assertEqual(capture.compositor_metadata(dump, (10, 20, 80, 40)), expected)
        with self.assertRaises(ValueError):
            capture.compositor_metadata(dump + dump, (10, 20, 80, 40))
        with self.assertRaises(ValueError):
            capture.compositor_metadata(dump, (0, 0, 80, 40))
        with self.assertRaises(ValueError):
            capture.compositor_metadata(dump.replace("w/h:10x4", "w/h:10x4x"), (10, 20, 80, 40))


if __name__ == "__main__":
    unittest.main()
