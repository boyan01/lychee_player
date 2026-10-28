#!/usr/bin/env python3
"""Small ISO/IEC 14496-30 subtitle fragments, which FFmpeg cannot mux."""
import pathlib
import struct
import sys


def box(kind, *parts):
    data = b''.join(parts)
    return struct.pack('>I4s', len(data) + 8, kind.encode()) + data


def fields(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def initialization(codec):
    config = (b'http://www.w3.org/ns/ttml\0\0\0' if codec == 'stpp'
              else box('vttC', b'WEBVTT'))
    entry = box(codec, bytes(6), b'\0\1', config)
    table = box('stbl', box('stsd', fields(0, 1), entry),
                box('stts', bytes(8)), box('stsc', bytes(8)),
                box('stsz', bytes(12)), box('stco', bytes(8)))
    handler = b'subt' if codec == 'stpp' else b'text'
    track = box('trak', box('tkhd', fields(3, 0, 0, 1), bytes(68)),
                box('mdia', box('mdhd', fields(0, 0, 0, 1000, 0, 0)),
                    box('hdlr', bytes(8), handler, bytes(12)),
                    box('minf', table)))
    return box('ftyp', b'iso6', bytes(4), b'iso6') + box(
        'moov', box('mvhd', fields(0, 0, 0, 1000), bytes(84)), track,
        box('mvex', box('trex', fields(0, 1, 1, 0, 0, 0))))


def fragment(sequence, samples):
    def moof(offset):
        rows = b''.join(fields(duration, len(payload)) for _, duration, payload in samples)
        return box('moof', box('mfhd', fields(0, sequence)), box(
            'traf', box('tfhd', fields(0x20000, 1)),
            box('tfdt', fields(0, samples[0][0])),
            box('trun', fields(0x301, len(samples), offset), rows)))
    data = b''.join(payload for _, _, payload in samples)
    return moof(len(moof(0)) + 8) + box('mdat', data)


def document(cues):
    paragraphs = ''.join(f'<p begin="{a}ms" end="{b}ms">{text}</p>' for a, b, text in cues)
    return ('<tt xmlns="http://www.w3.org/ns/ttml" '
            'xmlns:ttp="http://www.w3.org/ns/ttml#parameter" '
            'ttp:profile="http://www.w3.org/ns/ttml/profile/imsc1/text">'
            f'<body><div>{paragraphs}</div></body></tt>').encode()


def generate(root):
    folder = root / 'hls_fmp4_subtitles'
    folder.mkdir(parents=True, exist_ok=True)
    for codec in ('stpp', 'wvtt'):
        init = initialization(codec)
        if codec == 'stpp':
            groups = [[(0, 4000, document([(1000, 3000, 'Hello &amp; IMSC')]))],
                      [(4000, 8000, document([(8500, 9500, 'Later &lt;IMSC&gt;')]))]]
        else:
            empty = box('vtte')
            groups = [[(0, 1000, empty),
                       (1000, 2000, box('vttc', box('payl', b'Hello &amp; WebVTT'))),
                       (3000, 1000, empty)],
                      [(4000, 4500, empty),
                       (8500, 1000, box('vttc', box('payl', b'Later &lt;WebVTT&gt;'))),
                       (9500, 2500, empty)]]
        (folder / f'{codec}_init.mp4').write_bytes(init)
        joined = init
        for i, samples in enumerate(groups):
            data = fragment(i + 1, samples)
            joined += data
            (folder / f'{codec}_{i}.m4s').write_bytes(data)
        (folder / f'{codec}.mp4').write_bytes(joined)
        (folder / f'{codec}.m3u8').write_text(
            '#EXTM3U\n#EXT-X-TARGETDURATION:8\n#EXT-X-PLAYLIST-TYPE:VOD\n'
            f'#EXT-X-MAP:URI="{codec}_init.mp4"\n#EXTINF:4,\n{codec}_0.m4s\n'
            f'#EXTINF:8,\n{codec}_1.m4s\n#EXT-X-ENDLIST\n')
        (folder / f'{codec}_master.m3u8').write_text(
            '#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="English",'
            f'LANGUAGE="en",DEFAULT=YES,URI="{codec}.m3u8"\n'
            '#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,'
            'CODECS="avc1.640028,mp4a.40.2",SUBTITLES="subs"\n'
            '../hls_fmp4/index.m3u8\n')


if __name__ == '__main__':
    generate(pathlib.Path(sys.argv[1]))
