# lychee_player

A cross-platform media player with hardware decoding, built as a reusable
C/C++ library with a Flutter plugin on top.

> Status: early rewrite (v2). Plays on macOS, iOS, Android, Windows and Linux;
> see [docs/roadmap.md](docs/roadmap.md) for what is missing.

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
    backends/android/ AAudio output
    backends/windows/ WASAPI output
    backends/linux/   PulseAudio (and PipeWire's pulse server) output
    api/              C API implementation
examples/cli/         headless player for regression testing
examples/glfw/        windowed player without Flutter (GLFW + Metal, macOS)
flutter/              Flutter plugin (lib/; darwin/ for iOS and macOS, android/,
                      windows/, linux/) and its example app
```

Video frames leave the library as native handles (`CVPixelBufferRef` on Apple
platforms) or as CPU RGBA rows elsewhere. The caller pulls them per vsync with
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
`--no-hw`, `--cpu` (CPU BGRA frames), `--rgba` (CPU RGBA frames),
`--refresh HZ`, `--vsync-jitter US`, `--volume 0..1` (muted by default),
`--rate R [--rate-after SEC]`, `--loop`, `--open-after SEC URL` (reopen on
the same player), `--select TYPE:INDEX [--select-after SEC]` (TYPE is
`video`, `audio` or `subtitle`, INDEX a track index as the CLI lists them;
-1 turns the type off), `--virtual-time` (play on a simulated clock, as fast
as the media decodes, with audio to a null sink).

With GLFW installed (`brew install glfw`) the build also produces
`build/examples/glfw/lychee_glfw path/to/video.mp4`: a window that draws the
frames with Metal. Space pauses, left/right seek 5 s, escape quits.

`tool/regression.sh` generates a synthetic media set and runs the CLI over it
(`TSAN=1` for a ThreadSanitizer build). Contributors and agents: see
[AGENTS.md](AGENTS.md) and [docs/roadmap.md](docs/roadmap.md).

## Flutter plugin

The plugin lives in `flutter/`. Its iOS/macOS pod compiles `core/` through
the forwarders in `flutter/darwin/Classes/core`.
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
player.setRate(1.5);           // 0.25..4, pitch preserved
player.setLooping(true);
// An index into player.value.tracks; null turns the type off.
final sub = player.value.tracks
    .indexWhere((t) => t.type == LycheeTrackType.subtitle);
player.selectTrack(LycheeTrackType.subtitle, sub);
player.getMetadata('title');
// player.value: state, position, duration, tracks, subtitle, rate, ...
player.dispose();
```

Subtitles arrive as plain text in `player.value.subtitle`; the app draws
them.

The example app (`flutter/example`) turns off the macOS App Sandbox so it can
open arbitrary paths. It plays `--dart-define=LYCHEE_URL=...` on start.

The plugin compiles `core/` from outside its package directory, so for now it
can only be used as a path or git dependency, not from pub.dev.

### Per platform

- **macOS**: links the FFmpeg found by pkg-config (`brew install ffmpeg`).
- **iOS**: needs an LGPL FFmpeg built from source once (FFmpeg 9 source
  tree): `tool/build_ffmpeg.sh ios-sim-arm64 SRC`, `tool/build_ffmpeg.sh
  ios-sim-x86_64 SRC` (and `ios-arm64` for devices), then
  `tool/package_ffmpeg_apple.sh`, which writes the xcframeworks the pod
  vendors.
- **Android**: `tool/build_ffmpeg.sh android-arm64-v8a SRC` (and
  `android-x86_64`); the plugin's CMake links `third_party/ffmpeg/android-ABI`
  and builds only the ABIs found there. Needs minSdk 26 (AAudio).
- **Windows**: `flutter build windows`. The plugin downloads a pinned
  LGPL-only FFmpeg (BtbN, FFmpeg 9.0, LGPLv3) at configure time and ships its
  DLLs next to the app. To build the core and `examples/cli` alone: `cmake
  -DDEST=C:/ffmpeg -P flutter/windows/ffmpeg.cmake`, then `cmake -S . -B build
  -G "Visual Studio 17 2022" -A x64 -DLYCHEE_FFMPEG_ROOT=<printed dir>` and
  put `<dir>/bin` on `PATH` to run.
- **Linux**: `flutter build linux`. The plugin downloads a pinned LGPL
  FFmpeg (BtbN, 9.0, LGPLv3) into `~/.cache/lychee_player` on first
  configure and bundles it next to `liblychee_player.so`; set
  `LYCHEE_FFMPEG_ROOT` to use your own. Apps need `libgtk-3` and `libpulse0`
  at runtime. The core alone needs FFmpeg 8 or newer, which Ubuntu 22.04
  does not ship: point pkg-config at a prebuilt one
  (`PKG_CONFIG_PATH=<ffmpeg>/lib/pkgconfig`, `LD_LIBRARY_PATH=<ffmpeg>/lib`
  to run the CLI) and install `libpulse-dev`.

## License

MIT, see [LICENSE](LICENSE).

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. On macOS the
plugin links the FFmpeg found by pkg-config, which may be a GPL build
(Homebrew's is); that is for development only. The iOS and Android builds from
`tool/build_ffmpeg.sh` are LGPL 2.1+ (the script checks configure's license
line), the Windows and Linux downloads are LGPL 3+. Distributed apps must ship an
LGPL-only FFmpeg (no `--enable-gpl` / `--enable-nonfree`), linked
dynamically.
