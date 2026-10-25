# Roadmap

State as of 2026-10-25: the core plays files and network streams through
FFmpeg, with accurate seek, pull-based frame selection, playback rate
(WSOLA), looping and reopening, track selection, text subtitles and
metadata. The Flutter plugin runs on macOS and iOS (VideoToolbox), Android,
Windows and Linux (software decoding). Everything below is unfinished.

Each item says where the work goes and when it is done. Remove an item when it
is done; add one when you find a gap. Order within a section is priority.

## Unverified

Built, but never observed working. Cheap to close; do these before building
on top of them.

1. **iOS on a device**: the release example builds with the device FFmpeg
   (`tool/build_ffmpeg.sh ios-arm64`, then `tool/package_ffmpeg_apple.sh`),
   installs on an iPad Pro 11" (M1), launches and streams from
   `tool/serve_media.py` over the LAN (2026-10-25). Not yet seen on its
   screen: `hw` in the stats line (VideoToolbox), the frame count, audio
   session interruptions (a call) and route changes (unplugging
   headphones). The simulator decodes in software because VideoToolbox
   hwaccel fails there.
   On a busy Mac (load 4-7) the simulator showed 300-324 of 360 frames
   with `dropped 0`: the frame selection skips frames by cadence when the
   pull rate falls below the frame rate, and those do not count as
   dropped. Check the count on a device; if it is short there too, find
   out why the display link pulls slower than 60 Hz.

## Known defects

1. **Linux RSS grows on file switches**: about 40 MB per switch with
   glibc's default malloc, without a plateau over 30 switches; flat at
   ~740 MB with `MALLOC_ARENA_MAX=2`. Each player's new threads get their
   own malloc arenas, which keep the freed 1080p frames. Fix: pool frame
   buffers across players, or `malloc_trim` after a player is destroyed.
2. **Clock drift after a loop**: `lychee_cli --loop` reports `max_drift`
   around -60 ms after the jump back to the start, against ~-10 ms before
   (macOS and Windows). The audio clock is probably re-anchored before the
   first post-loop audio reaches the device. Done when drift after a loop
   matches drift before it, with a regression case asserting it.
3. **Position jumps back after an audio track switch** with software
   decoding (`--no-hw --select audio:2`: `backward_jumps=1`, every run on
   macOS and Windows; 0 with VideoToolbox). Done when the switch resumes
   without a backward jump and the regression case asserts
   `backward_jumps=0`.
4. **Glitches after a seek on Linux**: `seek-forward` shows exactly 3
   glitches in every WSL run (the case allows 3), 0 on macOS. Probably the
   PulseAudio stream's latency estimate settling after the flush. Done when
   it shows 0-1 there.

## Features

1. **HDR / 10-bit**: frames are converted to 8-bit BGRA, so HDR is clipped.
   Needs a 10-bit or YUV handle type plus color metadata across the C API,
   and EDR on macOS.
2. **Subtitles beyond plain text**: text cues (SRT, WebVTT, mov_text, ASS
   text with tags stripped) work. Missing: ASS styling and positioning
   (libass), bitmap subtitles (PGS, DVB, VobSub), and cues that started
   before a subtitle track is switched on mid-file (they appear only from
   the next cue).
3. **Artwork**: `lychee_player_get_metadata` returns text tags only;
   attached pictures are not exposed.

## Platforms

Every platform plays through FFmpeg software decoding and CPU RGBA frames
except Apple's (VideoToolbox, `CVPixelBuffer`). What is left per platform is
hardware decoding with zero-copy frames, a renderer in `examples/glfw`
(Metal only today), and its own `tool/regression.sh` run.

1. **Android hardware decoding**: today FFmpeg decodes in software, the
   plugin copies RGBA rows into the `SurfaceProducer`'s window on a
   Choreographer thread. On the Pixel 10 emulator (4 vCPUs) 1080p30 runs at
   30 fps with a few drops per second: the raster thread, the copy and the
   decoder together fill the CPU. Decode with MediaCodec straight into the
   `SurfaceProducer`'s Surface (FFmpeg's `*_mediacodec` decoders with
   `av_jni_set_java_vm`, or NDK `AMediaCodec`), which needs a frame type
   for "already on the Surface" and moves frame selection to release time.
   On a Xiaomi 14 (Snapdragon 8 Gen 3, Android 16) 1080p30 plays in
   software with 0 dropped frames in a debug app. AAudio route changes are
   untested.
2. **Windows hardware decoding**: D3D11VA, frames as D3D11 textures shared
   with Flutter's GPU surface texture instead of the `PixelBufferTexture`
   upload. The FFmpeg DLLs (BtbN's LGPLv3 build, ~120 MB) should become a
   slim build of our own. Unverified there: device switching, multiple
   monitors, audible output, `tool/regression.sh` (needs bash), and the
   integration test.
3. **Linux hardware decoding**: VAAPI decoding, dmabuf → EGLImage into
   `FlTextureGL` instead of the `FlPixelBufferTexture` copy. This is where
   media-kit had the most texture bugs; budget for it. The bundled FFmpeg
   (BtbN, LGPLv3, ~130 MB) should become a decode-only
   `tool/build_ffmpeg.sh linux-x86_64` build (smaller, LGPL 2.1). Unverified
   there: the Impeller renderer (the VM runs with it off) and a build from
   22.04 running on 26.04.
4. **Swift Package Manager**: add `flutter/darwin/lychee_player/Package.swift`;
   Flutter already warns that CocoaPods-only plugins will become an error.

## FFmpeg-free builds

The interfaces allow it (`LYCHEE_WITH_FFMPEG=OFF` compiles but cannot open
anything). Pieces, in order:

1. `DataSource` (file + HTTP on platform networking), and FFmpeg reading
   through it via a custom `AVIOContext`, so both builds share IO behaviour.
2. Direct VideoToolbox / AudioToolbox decoders. The hard part is output
   reordering: VideoToolbox emits decode order, so the reorder depth must
   come from an H.264/HEVC SPS parser (port Chromium's
   `media/parsers/h264_parser.cc`, `h265_parser.cc`).
3. Container parsers ported from Chromium (`media/formats/mp4`, `mp2t`,
   `webm`) and an HLS playlist parser.

Linux keeps FFmpeg: it has no system audio decoder or demuxer.

## Distribution and infrastructure

1. **LGPL FFmpeg**: build an LGPL-only FFmpeg per platform, bundle it as
   dynamic libraries/frameworks, and replace the podspec's pkg-config
   lookup. Required before any release.
2. **pub.dev packaging**: the plugin in `flutter/` compiles `../core` through
   forwarders, and `flutter pub publish` only packs `flutter/`, so today the
   plugin works only as a path or git dependency. Decide together with item 1
   (most likely: ship a prebuilt core next to the prebuilt FFmpeg). Done when
   `flutter pub publish --dry-run` produces a package that builds in a fresh
   app.
3. **Example app**: a file picker instead of a path field.
