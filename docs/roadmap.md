# Roadmap

State as of 2026-10-24 (commit 12c0538): a macOS MVP. The core plays local
files with VideoToolbox decoding (through FFmpeg's hwaccel), CoreAudio
output, accurate seek, and pull-based frame selection; the Flutter macOS
plugin shows it through a texture. Everything below is unfinished.

Each item says where the work goes and when it is done. Remove an item when it
is done; add one when you find a gap. Order within a section is priority.

## Unverified

Built, but never observed working. Cheap to close; do these before building
on top of them.

None as of 2026-10-24.

## Known defects

1. **Frame selection has no cadence awareness.**
   `VideoRenderer::SelectFrameLocked` picks the last frame with
   `pts <= media_time + half a frame`. When the frame rate equals the refresh
   rate, the phase can sit on that boundary and timing jitter alternates
   drops and repeats (the CLI showed 35 drops in 8 s of 60 fps video before
   it switched to ideal vsync timestamps). Port Chromium's
   `media/filters/video_renderer_algorithm.cc` and
   `video_cadence_estimator.cc`. Done when a regression case with jittered
   vsync timestamps (add a `--vsync-jitter` CLI option) plays 24/30/60 fps at
   60 Hz with no drops beyond the cadence's own.
2. **Seek and quit wait for blocking network reads.** `Player::Run` only sees
   a pending seek between `Demuxer::Read` calls; `FFmpegDemuxer`'s interrupt
   callback only reacts to `Abort`. Make a pending seek interrupt the read.
   Done when seeking an HTTP stream on a throttled connection responds within
   100 ms of the request.
3. **Errors are swallowed.** A decoder that returns a fatal error, or a
   demuxer read error, is treated as end of stream; the user sees `ended`.
   Report `LYCHEE_EVENT_ERROR` (or a recoverable buffering state for network
   errors). Done when a truncated file and a dropped connection produce
   distinguishable, documented outcomes.
4. **Buffering policy is fixed.** Read-ahead is 3 s / 64 MB
   (`kMinQueuedDuration`, `kMaxQueuedBytes`) and playback resumes at 200 ms
   of audio, with no hysteresis. On a slow network this stutters between
   ready and buffering. Done when resume needs a larger watermark than the
   starvation point, and `buffered_position` reflects real queued data.
5. **Video size only comes from the container.** A mid-stream resolution
   change (common in HLS) never emits `LYCHEE_EVENT_VIDEO_SIZE_CHANGED`.
   Detect it from decoded frames. Done when an adaptive stream switching
   renditions resizes the Flutter view.
6. **Audio output format is fixed at open.** If the default device changes
   (headphones unplugged) to a different sample rate, the resampler target
   and latency are stale. Listen for device changes in `CoreAudioSink`.
7. **Packets are copied twice** (FFmpeg packet → `EncodedPacket::data` →
   padded `AVPacket`). Give `EncodedPacket` a padded, ref-counted buffer.
   Matters for 4K high-bitrate streams.

## Features

1. **Playback rate** with pitch preserved: port Chromium's
   `media/filters/audio_renderer_algorithm.cc` (WSOLA) into
   `AudioRenderer`; the clock gains a rate. API: `lychee_player_set_rate`.
2. **Track selection**: list audio/video tracks from `MediaInfo`, switch at
   runtime (`Demuxer::SetEnabledStreams` exists; renderers must be
   re-initialized).
3. **Subtitles**: none yet. Text subtitles first (SRT, WebVTT, mov_text) as
   timed cues over the C API; rendering stays in the UI. ASS/libass later.
4. **HDR / 10-bit**: frames are converted to 8-bit BGRA, so HDR is clipped.
   Needs a 10-bit or YUV handle type plus color metadata across the C API,
   and EDR on macOS.
5. **Metadata and stats**: title/artist/artwork; richer `lychee_stats`
   (buffer levels, decode time).
6. **Looping**, and opening a new URL on an existing player (today a
   `Player` opens once; Flutter creates one per file).

## Platforms

Each needs a decoder path, an `AudioSink`, a `FrameConverter`, a Flutter
texture bridge, a renderer in `examples/glfw` (D3D11 on Windows, GL or
Vulkan with dmabuf import on Linux), and its own `tool/regression.sh` run.

1. **iOS**: `CoreAudioSink` already compiles a RemoteIO path but has no
   `AVAudioSession` handling and has never run. Plugin texture bridge is the
   same as macOS.
2. **Windows**: D3D11VA decoding, frames as D3D11 textures shared with
   Flutter's GPU surface texture, WASAPI output.
3. **Linux**: VAAPI decoding, dmabuf → EGLImage into `FlTextureGL`,
   PipeWire/PulseAudio output. This is where media-kit had the most texture
   bugs; budget for it.
4. **Android**: MediaCodec decoding to a Surface, AAudio output.
5. **Swift Package Manager**: add `flutter/macos/lychee_player/Package.swift`;
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
2. **CI**: run `tool/regression.sh` (and the TSan variant) on a macOS runner.
   Hardware decoding may be unavailable on CI runners; the script's `hw=1`
   expectations will need a software-only mode.
3. **Unit tests** for `MediaClock`, `PacketQueue`, and frame selection, which
   today are only tested end to end.
4. **pub.dev packaging**: the plugin in `flutter/` compiles `../core` through
   forwarders, and `flutter pub publish` only packs `flutter/`, so today the
   plugin works only as a path or git dependency. Decide together with item 1
   (most likely: ship a prebuilt core next to the prebuilt FFmpeg). Done when
   `flutter pub publish --dry-run` produces a package that builds in a fresh
   app.
5. **Example app**: a file picker instead of a path field.
