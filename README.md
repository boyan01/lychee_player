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
examples/glfw/        windowed player without Flutter (GLFW + Metal, macOS)
flutter/              Flutter plugin (lib/, macos/) and its example app
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

With GLFW installed (`brew install glfw`) the build also produces
`build/examples/glfw/lychee_glfw path/to/video.mp4`: a window that draws the
frames with Metal. Space pauses, left/right seek 5 s, escape quits.

`tool/regression.sh` generates a synthetic media set and runs the CLI over it
(`TSAN=1` for a ThreadSanitizer build). Contributors and agents: see
[AGENTS.md](AGENTS.md) and [docs/roadmap.md](docs/roadmap.md).

## Flutter plugin

The plugin lives in `flutter/`. Its macOS pod compiles `core/` through the
forwarders in `flutter/macos/Classes/core`.
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

The example app (`flutter/example`) turns off the macOS App Sandbox so it can
open arbitrary paths.

The plugin compiles `core/` from outside its package directory, so for now it
can only be used as a path or git dependency, not from pub.dev.

## License

MIT, see [LICENSE](LICENSE).

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. During
development the plugin links the FFmpeg found by pkg-config, which may be a
GPL build (Homebrew's is). Distributed apps must ship an LGPL-only FFmpeg
(no `--enable-gpl` / `--enable-nonfree`), linked dynamically.
