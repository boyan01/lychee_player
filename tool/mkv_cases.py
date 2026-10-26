#!/usr/bin/env python3
"""Rewrites a Matroska file into a test case for the FFmpeg-free demuxer.

    python3 -I tool/mkv_cases.py MODE IN OUT

MODE:
  no-cues        the Cues become a Void element: seeks scan the clusters
  unknown-sizes  every cluster's size is unknown, as live recorders write
  huge-tracks    the Tracks element claims 1 GiB
  huge-block     a video block in the second cluster claims 1 GiB; the
                 cluster's size is unknown, so the block is in bounds
  corrupt        noise from the middle of the file on
  truncated      the first 60% of the file
"""

import random
import sys


def vint(data, pos, keep_marker=False):
    """An EBML variable-length integer at pos: (value, length)."""
    first = data[pos]
    length = 1
    while length <= 8 and not first & (0x80 >> (length - 1)):
        length += 1
    value = first if keep_marker else first & (0xFF >> length)
    for i in range(1, length):
        value = (value << 8) | data[pos + i]
    return value, length


def size_field(value, length):
    """value as a size of the given length, with its marker bit."""
    return (value | (1 << (7 * length))).to_bytes(length, "big")


def unknown(length):
    return size_field((1 << (7 * length)) - 1, length)


def segment_children(data):
    """(id, offset, id length, size length, size) of each top-level element."""
    _, id_length = vint(data, 0, True)
    size, size_length = vint(data, id_length)
    pos = id_length + size_length + size  # past the EBML header
    _, id_length = vint(data, pos, True)
    _, size_length = vint(data, pos + id_length)
    pos += id_length + size_length  # into the Segment
    children = []
    while pos < len(data):
        element, id_length = vint(data, pos, True)
        size, size_length = vint(data, pos + id_length)
        children.append((element, pos, id_length, size_length, size))
        if size == (1 << (7 * size_length)) - 1:
            break
        pos += id_length + size_length + size
    return children


CLUSTER = 0x1F43B675
CUES = 0x1C53BB6B
TRACKS = 0x1654AE6B
SIMPLE_BLOCK = 0xA3
VOID = 0xEC


def main():
    mode, source, target = sys.argv[1:4]
    data = bytearray(open(source, "rb").read())
    children = segment_children(data)
    if mode == "no-cues":
        for element, pos, id_length, size_length, size in children:
            if element == CUES:
                # One ID byte instead of four: the size field takes three
                # more, so the element keeps its length.
                length = id_length - 1 + size_length
                data[pos:pos + id_length + size_length] = (
                    bytes([VOID]) + size_field(size, length))
    elif mode == "unknown-sizes":
        for element, pos, id_length, size_length, _ in children:
            if element == CLUSTER:
                data[pos + id_length:pos + id_length + size_length] = unknown(
                    size_length)
    elif mode == "huge-tracks":
        # In an eight-byte size field: what follows moves, but the
        # demuxer stops at the size.
        for element, pos, id_length, size_length, _ in children:
            if element == TRACKS:
                data[pos + id_length:pos + id_length + size_length] = size_field(
                    1 << 30, 8)
    elif mode == "huge-block":
        clusters = [c for c in children if c[0] == CLUSTER]
        _, pos, id_length, size_length, _ = clusters[1]
        data[pos + id_length:pos + id_length + size_length] = unknown(size_length)
        pos += id_length + size_length
        while True:
            element, id_length = vint(data, pos, True)
            size, size_length = vint(data, pos + id_length)
            body = pos + id_length + size_length
            if element == SIMPLE_BLOCK and data[body] == 0x81:  # track 1
                data[pos + id_length:body] = size_field(1 << 30, 8)
                break
            pos = body + size
    elif mode == "corrupt":
        rng = random.Random(1)
        middle = len(data) // 2
        data[middle:] = bytes(rng.randrange(256) for _ in range(len(data) - middle))
    elif mode == "truncated":
        del data[len(data) * 6 // 10:]
    else:
        sys.exit("unknown mode: " + mode)
    open(target, "wb").write(data)


if __name__ == "__main__":
    main()
