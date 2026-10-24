# lychee_player

A cross-platform media player with hardware decoding, built as a reusable
C/C++ library with a Flutter plugin on top.

> Status: early rewrite (v2). Only macOS works for now.

## Layout

```
core/                 C++ core, no UI toolkit dependency
  include/lychee/     lychee.h, the only public header (C ABI)
  src/
    media/            backend-neutral interfaces: Demuxer, VideoDecoder,
                      AudioDecoder, AudioSink, FrameConverter
    pipeline/         Player state machine, MediaClock, audio/video renderers
    backends/ffmpeg/  FFmpeg demuxer and decoders (optional, LYCHEE_WITH_FFMPEG)
    backends/apple/   CoreAudio output, CVPixelBuffer conversion
    api/              C API implementation
examples/cli/         headless player for regression testing
lib/, macos/          Flutter plugin
example/              Flutter example app
```

Video frames leave the library as native handles (`CVPixelBufferRef` on Apple
platforms). The caller pulls them per vsync with
`lychee_player_has_new_frame()` / `lychee_player_acquire_frame()`, and the
library picks the frame that should be on screen at that time.

## Building the core

Requires CMake, Ninja, pkg-config and FFmpeg development files
(`brew install cmake ninja pkg-config ffmpeg`).

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/examples/cli/lychee_cli --seconds 5 path/to/video.mp4
```

`lychee_cli` pulls frames at a simulated 60 Hz and reports state, position,
dropped frames and clock drift. Options: `--seek SEC [--seek-after SEC]`,
`--no-hw`, `--cpu`, `--volume 0..1` (muted by default).

## Flutter plugin

The macOS pod compiles `core/` through the forwarders in `macos/Classes/core`.
Regenerate them after adding or removing core sources:

```sh
python3 tool/generate_forwarders.py
```

```dart
final player = await LycheePlayer.create();
player.open('/path/to/video.mp4');
player.play();
// ...
LycheeVideo(player);
// ...
player.dispose();
```

The example app turns off the macOS App Sandbox so it can open arbitrary
paths.

## License

MIT, see [LICENSE](LICENSE).

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. During
development the plugin links the FFmpeg found by pkg-config, which may be a
GPL build (Homebrew's is). Distributed apps must ship an LGPL-only FFmpeg
(no `--enable-gpl` / `--enable-nonfree`), linked dynamically.
