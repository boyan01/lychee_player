#!/usr/bin/env python3
"""Writes a PGS (Blu-ray .sup) subtitle stream for tests: two cues, each a
white box with a black border on a 1920x1080 canvas, so a player's
picture can be checked by position and colour.

  tool/make_pgs.py OUT.sup

FFmpeg has no PGS encoder; tool/regression.sh muxes this file into
Matroska and turns it into DVB and VobSub subtitles from there.
"""
import struct
import sys

WIDTH, HEIGHT = 1920, 1080
# (start s, end s, x, y, width, height)
CUES = [(1.0, 2.5, 660, 900, 600, 80), (3.0, 4.5, 860, 100, 200, 60)]


def segment(kind, pts, payload):
    ticks = int(pts * 90000)
    return b"PG" + struct.pack(">IIBH", ticks, ticks, kind, len(payload)) + payload


def rle(width, height, pixel):
    """PGS run-length coding of rows of palette indices."""
    out = bytearray()
    for y in range(height):
        row = [pixel(x, y) for x in range(width)]
        x = 0
        while x < width:
            color, run = row[x], 1
            while x + run < width and row[x + run] == color and run < 16383:
                run += 1
            if color != 0 and run < 3:
                out += bytes([color]) * run
            elif color == 0:
                out += bytes([0, run]) if run < 64 else bytes([0, 0x40 | run >> 8, run & 0xFF])
            else:
                out += bytes([0, 0x80 | run, color]) if run < 64 else bytes([0, 0xC0 | run >> 8, run & 0xFF, color])
            x += run
        out += b"\0\0"
    return bytes(out)


def display_set(number, pts, cue):
    if cue is None:
        # Clears the screen: no objects.
        pcs = struct.pack(">HHBHBBBB", WIDTH, HEIGHT, 0x10, number, 0x00, 0, 0, 0)
        wds = struct.pack(">BBHHHH", 1, 0, 0, 0, 1, 1)
        return segment(0x16, pts, pcs) + segment(0x17, pts, wds) + segment(0x80, pts, b"")
    _, _, x, y, w, h = cue
    pcs = struct.pack(">HHBHBBBB", WIDTH, HEIGHT, 0x10, number, 0x80, 0, 0, 1)
    pcs += struct.pack(">HBBHH", 0, 0, 0, x, y)
    wds = struct.pack(">BBHHHH", 1, 0, x, y, w, h)
    # Palette entries: index, Y, Cr, Cb, alpha. 1 white, 2 black.
    pds = struct.pack(">BB", 0, 0) + bytes([1, 235, 128, 128, 255, 2, 16, 128, 128, 255])
    border = 6
    data = rle(w, h, lambda px, py: 2 if min(px, py, w - 1 - px, h - 1 - py) < border else 1)
    ods = struct.pack(">HBB", 0, 0, 0xC0) + struct.pack(">I", len(data) + 4)[1:] + struct.pack(">HH", w, h) + data
    return (segment(0x16, pts, pcs) + segment(0x17, pts, wds) + segment(0x14, pts, pds)
            + segment(0x15, pts, ods) + segment(0x80, pts, b""))


def main():
    # An empty display set at 0, or FFmpeg moves the cues to start there.
    out = bytearray(display_set(0, 0.0, None))
    number = 1
    for cue in CUES:
        out += display_set(number, cue[0], cue)
        out += display_set(number + 1, cue[1], None)
        number += 2
    with open(sys.argv[1], "wb") as f:
        f.write(out)


main()
