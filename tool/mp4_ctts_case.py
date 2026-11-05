#!/usr/bin/env python3
"""Drops an MP4's composition offsets (ctts), as some files with B-frames
have none: every sample's pts is then its dts, in decode order.

The table becomes a free box, so no chunk offset moves. The first edit
list's media time (the video track's, which skips the offsets' delay) is
set to zero, so the file's frames keep the times they had.

Usage: mp4_ctts_case.py IN OUT
"""
import struct
import sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts'}


def boxes(data, start, end):
    """Offset, size and kind of each box in data[start:end], depth first."""
    while start + 8 <= end:
        size, kind = struct.unpack('>I4s', data[start:start + 8])
        if size < 8:
            return
        yield start, size, kind
        if kind in CONTAINERS:
            yield from boxes(data, start + 8, start + size)
        start += size


def main():
    source, target = sys.argv[1], sys.argv[2]
    data = bytearray(open(source, 'rb').read())
    found = list(boxes(data, 0, len(data)))
    ctts = [start for start, _, kind in found if kind == b'ctts']
    edits = [start for start, _, kind in found if kind == b'elst']
    if len(ctts) != 1 or not edits:
        sys.exit('want one ctts and an edit list in ' + source)
    data[ctts[0] + 4:ctts[0] + 8] = b'free'
    elst = edits[0]
    if data[elst + 8] != 0:
        sys.exit('want a version 0 edit list in ' + source)
    # version and flags, entry_count, segment_duration, then media_time.
    data[elst + 20:elst + 24] = struct.pack('>i', 0)
    open(target, 'wb').write(data)


if __name__ == '__main__':
    main()
