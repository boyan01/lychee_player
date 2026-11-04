#!/usr/bin/env python3
"""Drops the first entries of an MP4's sync sample table (stss).

Some MP4s list only later keyframes, so a demuxer that trusts the table
sees none at the start. The file keeps its size: the bytes the table
loses become a free box after it, so no chunk offset moves.

Usage: mp4_stss_case.py IN OUT COUNT
"""
import struct
import sys


def find(data, kind, start, end):
    """Offset and size of the first |kind| box in data[start:end]."""
    containers = {b'moov', b'trak', b'mdia', b'minf', b'stbl'}
    while start + 8 <= end:
        size, box = struct.unpack('>I4s', data[start:start + 8])
        if size < 8:
            return None
        if box == kind:
            return start, size
        if box in containers:
            found = find(data, kind, start + 8, start + size)
            if found:
                return found
        start += size
    return None


def main():
    source, target, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    data = bytearray(open(source, 'rb').read())
    found = find(data, b'stss', 0, len(data))
    if not found:
        sys.exit('no stss in ' + source)
    start, size = found
    entries = struct.unpack('>I', data[start + 12:start + 16])[0]
    # The free box needs 8 bytes: at least two entries go.
    if count < 2 or count >= entries:
        sys.exit('stss has %d entries; cannot drop %d' % (entries, count))
    kept = data[start + 16 + 4 * count:start + size]
    table = (struct.pack('>I4sII', size - 4 * count, b'stss', 0,
                         entries - count) + kept)
    free = struct.pack('>I4s', 4 * count, b'free') + bytes(4 * count - 8)
    data[start:start + size] = table + free
    open(target, 'wb').write(data)


if __name__ == '__main__':
    main()
