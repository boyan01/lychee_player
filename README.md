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
    backends/apple/   CoreAudio output, CVPixelBuffer conversion; without
                      FFmpeg also VideoToolbox/AudioToolbox decoders, HTTP
    backends/mp4/     MP4 demuxer for builds without FFmpeg
    backends/android/ AAudio output
    backends/windows/ WASAPI output
    backends/linux/   PulseAudio (and PipeWire's pulse server) output
    api/              C API implementation
examples/cli/         headless player for regression testing
examples/glfw/        windowed player without Flutter (GLFW + Metal, macOS)
flutter/              Flutter plugin (lib/; darwin/ for iOS and macOS, android/,
                      windows/, linux/) and its example app
sdk/apple/            Swift package for macOS and iOS apps, and its example app
sdk/android/          Android library (AAR) for Android apps, and its example app
rust/                 lychee-sys (raw bindings, builds the core) and lychee (safe API)
```

Video frames leave the library as native handles (`CVPixelBufferRef` on Apple
platforms) or as CPU RGBA rows elsewhere. The caller pulls them per vsync with
`lychee_player_has_new_frame()` / `lychee_player_acquire_frame()`, and the
library picks the frame that should be on screen at that time.

Besides files and URLs, the library plays a live input
(`lychee_player_open_live()`): the caller declares H.264/HEVC and AAC/ALAC
tracks and pushes the packets a sender (e.g. screen mirroring) sends,
with the sender's timestamps; the library presents them a fixed latency
after they arrive and adapts to jitter and the audio device's latency.

## Building the core

Requires CMake, Ninja, pkg-config and FFmpeg development files
(`brew install cmake ninja pkg-config ffmpeg`). `-DLYCHEE_WITH_LIBASS=ON`
renders styled ASS subtitles with libass (`brew install libass`);
`tool/regression.sh` turns it on where libass is installed. On macOS and
iOS, `-DLYCHEE_WITH_FFMPEG=OFF` builds without FFmpeg: MP4 files and
http(s) URLs with H.264/HEVC (VideoToolbox) and AAC/ALAC (AudioToolbox);
`NO_FFMPEG=1 tool/regression.sh` tests it.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/examples/cli/lychee_cli --seconds 5 path/to/video.mp4
```

`lychee_cli` pulls frames at a simulated 60 Hz and reports state, position,
dropped frames and clock drift. Options: `--seek SEC [--seek-after SEC]`,
`--no-hw`, `--cpu` (CPU BGRA frames), `--rgba` (CPU RGBA frames),
`--native` (the decoder's own `CVPixelBuffer`s, 10-bit for HDR),
`--refresh HZ`, `--vsync-jitter US`, `--volume 0..1` (muted by default),
`--rate R [--rate-after SEC]`, `--loop`, `--open-after SEC URL` (reopen on
the same player), `--select TYPE:INDEX [--select-after SEC]` (TYPE is
`video`, `audio` or `subtitle`, INDEX a track index as the CLI lists them;
-1 turns the type off), `--virtual-time` (play on a simulated clock, as fast
as the media decodes, with audio to a null sink). `--live` pushes the
file's packets into a live input when a sender would send them, with
`--latency MS`, `--live-jitter MS`, `--live-stall SEC FOR`,
`--live-flush SEC` and `--live-gap SEC FOR` to simulate the network.

With GLFW installed (`brew install glfw`) the build also produces
`build/examples/glfw/lychee_glfw path/to/video.mp4`: a window that draws the
frames with Metal. Space pauses, left/right seek 5 s, escape quits.

`tool/regression.sh` generates a synthetic media set and runs the CLI over it
(`TSAN=1` for a ThreadSanitizer build). Contributors and agents: see
[AGENTS.md](AGENTS.md) and [docs/roadmap.md](docs/roadmap.md).

## Apple frameworks

The Flutter plugin's iOS/macOS pod and the Swift package both link prebuilt
xcframeworks from `flutter/darwin/Frameworks`: `lychee.xcframework` (the
core, a dynamic framework carrying only `lychee/lychee.h`) and an LGPL-only
FFmpeg. Build them once, and rebuild `lychee.xcframework` after changing
`core/` (FFmpeg 9 source tree as `SRC`):

```sh
for p in macos-arm64 macos-x86_64 ios-arm64 ios-sim-arm64 ios-sim-x86_64; do
  tool/build_ffmpeg.sh $p SRC
done
tool/package_ffmpeg_apple.sh   # FFmpeg xcframeworks
tool/build_apple.sh            # lychee.xcframework
```

Each xcframework has a universal macOS slice, an iOS device slice and one
simulator slice with both arm64 and x86_64. Leave out platforms you do not
need; the last two scripts package the ones that were built. The x86_64
macOS FFmpeg uses assembly only if `nasm` is installed.

## Swift package

`sdk/apple` is a Swift package (`LycheePlayer`, macOS 11 / iOS 16) for apps
without Flutter. Add it as a local package; it takes the xcframeworks above
as binary targets.

```swift
import LycheePlayer

let player = LycheePlayer()
player.onEvent = { event in print(event) }   // on the main queue
player.open("/path/to/video.mp4")
player.play()
let view = LycheePlayerView(frame: bounds)   // NSView / UIView
view.player = player
// player.state, position, duration, tracks, subtitle, stats, ...
```

`LycheePlayerView` pulls frames per vsync while it is in a window and shows
them aspect-fit and rotated, HDR in EDR (macOS 14, iOS 17 and later). To
draw frames yourself (Metal, a Flutter texture), use `LycheeFrameDriver`: it
calls back on the main queue when the next vsync shows a new frame, and
`copyFrame()` returns that `CVPixelBuffer`, in the decoder's format (YCbCr,
10-bit for HDR, with color attachments).

The example app (`sdk/apple/Example/Example.xcodeproj`, one target for macOS
and iOS) plays `LYCHEE_URL`, default `/tmp/lychee-media/h264_aac.mp4`:

```sh
xcodebuild -project sdk/apple/Example/Example.xcodeproj -scheme Example \
  -destination 'platform=macOS' -derivedDataPath build/sdk-apple build
build/sdk-apple/Build/Products/Debug/Example.app/Contents/MacOS/Example
```

For the iOS simulator use `-destination 'generic/platform=iOS Simulator'`,
`xcrun simctl install booted .../Debug-iphonesimulator/Example.app` and
`SIMCTL_CHILD_LYCHEE_URL=... xcrun simctl launch booted tech.soit.lychee.example`;
the simulator reads `/tmp` of the Mac.

## Android library

`sdk/android` builds one AAR, `tech.soit.lychee:lychee` (minSdk 26): the
core as `liblychee.so` with the LGPL FFmpeg next to it (build it first:
`tool/build_ffmpeg.sh android-arm64-v8a SRC MBEDTLS_SRC`, with an Mbed TLS
3.6 release for https, and `android-x86_64` for x86
emulators), and Kotlin classes over it. The core is also a prefab module
(`lychee`, header `lychee/lychee.h`) for native code that uses the C ABI.

```sh
sdk/android/gradlew -p sdk/android :lychee:publish   # into sdk/android/build/repo
```

```kotlin
val player = LycheePlayer()
player.addListener { event -> Log.d("lychee", "$event") }   // main thread
player.open("https://example.com/video.mp4")
player.play()
val view = LycheePlayerView(context)   // a SurfaceView, aspect-fit
view.player = player
// player.state, positionMs, durationMs, tracks, subtitle, stats, ...
view.player = null
player.close()
```

MediaCodec renders the video upright into the `SurfaceView`
(`lychee_config.rotate_surface_frames`). To show frames in a Surface of
your own, use `LycheeFrameDriver`: it pulls frames per vsync and asks for a
new Surface when frames switch between MediaCodec and software decoding.

The example app (`sdk/android/example`, `:example:assembleDebug`) plays the
`url` intent extra, by default `h264_aac.mp4` from `tool/serve_media.py`
through `adb reverse tcp:8765 tcp:8765`; `--ez hw false` decodes in
software:

```sh
adb shell am start -n tech.soit.lychee.example/.MainActivity --es url http://127.0.0.1:8765/rotated.mp4
```

## Rust

`rust/` is a Cargo workspace: `lychee-sys` holds the raw bindings
(generated from `lychee.h` by `tool/rust_bindings.sh` and checked in, so
no libclang is needed) and links `liblychee` as a shared library, and
`lychee` is the safe API over it. `lychee-sys` builds the core with CMake
(the tools from "Building the core"); FFmpeg comes from pkg-config, or
from an LGPL build with `LYCHEE_FFMPEG_ROOT=third_party/ffmpeg/PLATFORM`.
`LYCHEE_LIB_DIR` links a prebuilt `liblychee` instead. Dependents find the
library's directory in `DEP_LYCHEE_LIB_DIR`, to ship it next to their
binary. On macOS `liblychee` and the FFmpeg libraries have `@rpath`
install names: copy `liblychee.dylib` and the five FFmpeg libraries
(`libavcodec.63.dylib`, ...) next to the binary and link it with
`-C link-arg=-Wl,-rpath,@executable_path` (`@executable_path/../Frameworks`
in an app bundle).

```rust
let player = lychee::Player::new(&lychee::Config::default())?;  // CPU BGRA frames
player.on_event(move |event| { let _ = sender.send(event); });  // control thread
player.open("/path/to/video.mp4")?;
player.play();
// once per vsync:
let display_time = lychee::now_us() + vsync_interval_us;
if player.has_new_frame(display_time) {
    if let Some(frame) = player.acquire_frame(display_time) {
        let (pixels, stride) = frame.data().unwrap();  // released on drop
    }
}
```

`Player` is `Send + Sync`; a `Frame` is `Send` and may outlive the player.
The example plays a file headless and can save a frame as PPM:

```sh
cd rust && cargo run --example play -- /tmp/lychee-media/h264_aac.mp4 /tmp/frame.ppm
```

## Flutter plugin

The plugin lives in `flutter/`; its iOS/macOS pod vendors the Apple
frameworks above.

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
// player.value: state, position, duration, tracks, subtitle, artwork, ...
player.dispose();
```

Text subtitles arrive as plain text in `player.value.subtitle`; the app
draws them. Bitmap subtitles (PGS, DVB, VobSub), and styled ASS where the
core has libass, arrive as pictures in `player.value.subtitlePictures`,
which `LycheeVideo` draws over the video.

The example app (`flutter/example`) turns off the macOS App Sandbox so it can
open arbitrary paths. It plays `--dart-define=LYCHEE_URL=...` on start.

The Windows and Linux plugins compile `core/` from outside the package
directory, and the Apple frameworks and the Android AAR are built locally,
so for now the plugin can only be used as a path or git dependency, not
from pub.dev.

### Per platform

- **macOS and iOS**: the xcframeworks above; macOS 11 and iOS 16 or newer.
  The current SDK's libc++ supports nothing older on macOS, and the core
  converts frames with `VTPixelTransferSession` (iOS 16).
- **Android**: the AAR from the Android library above, published into
  `sdk/android/build/repo`, where the plugin looks for it; rerun the
  publish after changing `core/`. It has the ABIs FFmpeg was built for.
  Needs minSdk 26 (AAudio).
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

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. The core's
own CMake build (the CLI, the GLFW example, `tool/regression.sh`) links the
FFmpeg found by pkg-config, which may be a GPL build (Homebrew's is); that
is for development only. The macOS and iOS builds from
`tool/build_ffmpeg.sh` are LGPL 2.1+ (the script checks configure's license
line); the Android build is LGPL 3+, because it links mbedTLS (Apache-2.0)
for https, and so are the Windows and Linux downloads. Distributed apps
must ship an LGPL-only FFmpeg (no `--enable-gpl` / `--enable-nonfree`),
linked dynamically.

`tool/build_ffmpeg.sh` builds only what the core plays: MP4/MOV,
Matroska/WebM, MPEG-TS, HLS, FLV, Ogg and raw audio files; H.264, HEVC,
VP8, VP9 and AV1 (AV1 only through VideoToolbox: FFmpeg has no AV1
software decoder of its own), AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC-3,
E-AC-3 and PCM; SRT, WebVTT, mov_text, ASS, PGS, DVB and VobSub
subtitles; file, http(s)
and AES-128 HLS. Other formats need an entry in its lists.
