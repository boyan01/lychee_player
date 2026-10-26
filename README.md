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
    backends/mp4/     MP4 demuxer and tx3g text for builds without FFmpeg
    backends/mp2t/    MPEG-TS demuxer for builds without FFmpeg
    backends/hls/     HLS demuxer (TS segments) for builds without FFmpeg
    backends/android/ AAudio output
    backends/windows/ WASAPI output
    backends/linux/   PulseAudio (and PipeWire's pulse server) output
    api/              C API implementation
  fuzz/               libFuzzer target over the MP4 demuxer (tool/fuzz.sh)
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
iOS, `-DLYCHEE_WITH_FFMPEG=OFF` builds without FFmpeg: MP4 (fragmented
too), Matroska/WebM, MPEG-TS and HLS (TS segments), from files and http(s)
URLs, with
H.264/HEVC (VideoToolbox), AAC/ALAC (AudioToolbox) and tx3g subtitles;
`NO_FFMPEG=1 tool/regression.sh` tests it. On Windows it plays MP4 files
with H.264/HEVC and AAC through Media Foundation, on a GPU only.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/examples/cli/lychee_cli --seconds 5 path/to/video.mp4
```

`lychee_cli` pulls frames at a simulated 60 Hz and reports state, position,
dropped frames and clock drift. Options: `--seek SEC [--seek-after SEC]`,
`--no-hw`, `--cpu` (CPU BGRA frames), `--rgba` (CPU RGBA frames),
`--native` (the decoder's own `CVPixelBuffer`s, 10-bit for HDR),
`--d3d11` (Windows: shared D3D11 textures, as the Flutter plugin takes them),
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
(`TSAN=1` for a ThreadSanitizer build). `tool/fuzz.sh` fuzzes the
FFmpeg-free MP4 parser with libFuzzer under ASan and UBSan
(`FUZZ_SECONDS`, `JOBS`); it needs Homebrew's LLVM (`brew install llvm`,
a development tool only), since Apple's clang has no libFuzzer runtime.
Inputs it once crashed on live in `core/fuzz/regressions`, which the unit
tests replay. Contributors and agents: see
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

`-Plychee.ffmpeg=false` builds the core without FFmpeg: MP4 files only
(no http), decoded by MediaCodec (roadmap, FFmpeg-free builds).

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

Outside this repository (from pub.dev, or a git dependency without local
builds), the plugin takes the Apple frameworks and the Android AAR from the
GitHub release that `flutter/prebuilt.json` pins, downloaded into
`~/.cache/lychee_player` and checked against their SHA-256s;
`LYCHEE_PREBUILT_URL` names a mirror of the release URL. Inside it, local
builds win: the xcframeworks in `flutter/darwin/Frameworks` and the AAR in
`sdk/android/build/repo`. Windows and Linux compile the repository's
`core/`, or in the published package the copy `tool/release.sh` puts into
`flutter/core` while pub packs it.

### Per platform

- **macOS and iOS**: the xcframeworks above; macOS 11 and iOS 16 or newer.
  The current SDK's libc++ supports nothing older on macOS, and the core
  converts frames with `VTPixelTransferSession` (iOS 16).
- **Android**: the AAR from the Android library above, published into
  `sdk/android/build/repo`, where the plugin looks for it first; rerun the
  publish after changing `core/`. It has the ABIs FFmpeg was built for.
  Needs minSdk 26 (AAudio).
- **Windows**: `flutter build windows`. The plugin downloads the FFmpeg
  that `flutter/prebuilt.json` pins (`ffmpeg-windows-x86_64.zip`, the
  release's decode-only LGPL 2.1 build, 8 MB of DLLs; BtbN's LGPLv3 one
  while the pinned release has none) at configure time and ships its DLLs
  next to the app; `LYCHEE_FFMPEG_ROOT` (CMake or environment) names a
  local one instead, e.g. from `tool/build_ffmpeg.sh windows-x86_64 SRC`,
  which cross-compiles on Linux (`mingw-w64`, `libz-mingw-w64-dev`,
  `nasm`). To build the core and `examples/cli` alone: `cmake
  -DDEST=C:/ffmpeg -P flutter/windows/ffmpeg.cmake`, then `cmake -S . -B build
  -G "Visual Studio 17 2022" -A x64 -DLYCHEE_FFMPEG_ROOT=<printed dir>` and
  put `<dir>/bin` on `PATH` to run.
- **Linux**: `flutter build linux`. The plugin downloads a pinned LGPL
  FFmpeg (BtbN, 9.0, LGPLv3) into `~/.cache/lychee_player` on first
  configure and bundles it next to `liblychee_player.so`; set
  `LYCHEE_FFMPEG_ROOT` to use your own, e.g. the decode-only
  `tool/build_ffmpeg.sh linux-x86_64 SRC` (needs `nasm`, `libva-dev`,
  `libdrm-dev` and `libssl-dev`; build it on the oldest distribution you
  support; apps then also need `libva2`, `libva-drm2` and `libssl3`). Apps
  need `libgtk-3` and `libpulse0` at runtime. The core alone needs FFmpeg 8 or newer, which Ubuntu 22.04
  does not ship: point pkg-config at a prebuilt one
  (`PKG_CONFIG_PATH=<ffmpeg>/lib/pkgconfig`, `LD_LIBRARY_PATH=<ffmpeg>/lib`
  to run the CLI) and install `libpulse-dev`.

## Releases

A release is one run of `.github/workflows/release.yml`, after a
`## VERSION` section in `flutter/CHANGELOG.md`:

```sh
gh workflow run release.yml -f version=1.0.0
```

It builds the Apple xcframeworks, the Android AAR and the Windows FFmpeg
(cross-compiled on Ubuntu), pins them in
`flutter/prebuilt.json` and `Package.swift` (which lets SwiftPM take
`sdk/apple` by URL), builds a fresh app against the package pub would
publish, drafts the GitHub release with the assets and the FFmpeg and
Mbed TLS sources, and pushes the commit `Release VERSION` and the tag
`vVERSION`. The tag starts `publish.yml`, which publishes the release and
the package on pub.dev. Each step is a `tool/release.sh` command and runs
locally too:

```sh
tool/release.sh version 1.0.0
tool/release.sh build apple /tmp/sources && tool/release.sh pack apple /tmp/dist
tool/release.sh build android /tmp/sources && tool/release.sh pack android /tmp/dist
tool/release.sh build windows /tmp/sources && tool/release.sh pack windows /tmp/dist  # on Linux
tool/release.sh sources /tmp/dist && tool/release.sh pin /tmp/dist
tool/release.sh check /tmp/dist   # then git checkout the pins
```

Once per repository: a deploy key with write access whose private key is
the secret `RELEASE_DEPLOY_KEY` (the workflow's own token cannot start
`publish.yml`), a GitHub environment `pub.dev`, and automated publishing
on the package's pub.dev Admin page (repository `boyan01/lychee_player`,
tag pattern `v{{version}}`, environment `pub.dev`).

## License

MIT, see [LICENSE](LICENSE).

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. The core's
own CMake build (the CLI, the GLFW example, `tool/regression.sh`) links the
FFmpeg found by pkg-config, which may be a GPL build (Homebrew's is); that
is for development only. The macOS and iOS builds from
`tool/build_ffmpeg.sh` are LGPL 2.1+ (the script checks configure's license
line), and so is the Windows build (Schannel for https); the Android build
is LGPL 3+, because it links mbedTLS (Apache-2.0) for https, and so are
the Linux download (BtbN's) and the Linux build (OpenSSL 3). Distributed apps
must ship an LGPL-only FFmpeg (no `--enable-gpl` / `--enable-nonfree`),
linked dynamically.

[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) has the notices an app must
ship with lychee_player (Chromium, FFmpeg, Mbed TLS, and their license
texts). The prebuilt artifacts carry it: `lychee.framework` and, on
Android, `META-INF/lychee/THIRD_PARTY_NOTICES` in the APK.

`tool/build_ffmpeg.sh` builds only what the core plays: MP4/MOV,
Matroska/WebM, MPEG-TS, HLS, FLV, Ogg and raw audio files; H.264, HEVC,
VP8, VP9 and AV1 (AV1 only through VideoToolbox: FFmpeg has no AV1
software decoder of its own), AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC-3,
E-AC-3 and PCM; SRT, WebVTT, mov_text, ASS, PGS, DVB and VobSub
subtitles; file, http(s)
and AES-128 HLS. Other formats need an entry in its lists.
