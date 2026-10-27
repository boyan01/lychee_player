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
  fuzz/               libFuzzer targets over the parsers (tool/fuzz.sh)
examples/cli/         headless player for regression testing
examples/glfw/        windowed player without Flutter (GLFW + Metal on macOS,
                      Direct3D 11 on Windows, OpenGL ES on Linux)
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
renders styled ASS subtitles with libass (`brew install libass`, or
`-DLYCHEE_LIBASS_ROOT=` a build from `tool/build_libass.sh`);
`tool/regression.sh` turns it on where libass is installed, or with
`LIBASS_ROOT=third_party/libass/macos-arm64`. On macOS and
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
`--subtitle-ppm FILE` writes each subtitle with pictures (bitmap
subtitles, styled ASS) onto a grey canvas of the video's size.
`--frame-ppm SEC:FILE` (repeatable) writes the frame on screen SEC seconds
into the run, upright, as a binary PPM (CPU frames as they are,
`CVPixelBuffer`s through VideoToolbox to SDR BT.709, D3D11 textures through
a staging copy) and prints `snapshot FILE pts=US size=WxH`.

With GLFW installed (`brew install glfw`) the build also produces
`build/examples/glfw/lychee_glfw path/to/video.mp4`: a window that draws the
frames with Metal. Space pauses, left/right seek 5 s, escape quits.
`--seconds N` quits after N seconds and `--key SEC:NAME` (`space`, `left`,
`right`, `escape`) presses a key at SEC seconds, for unattended runs; it
then prints a summary (decoder, frames rendered and dropped, position).
`--size WxH` sets the window's client size (in points on macOS, where the
drawable is that times the display's scale), and `--snapshot SEC:FILE`
(repeatable; after any `--key` due at the same time) reads back what the
window drew once a frame is on screen and writes it as a binary PPM,
printing `snapshot FILE pts=US size=WxH video=X,Y,W,H` (the rectangle the
frame was fitted into). On
Windows the same program (`main_win.cc`) draws with Direct3D 11 the frames
the Flutter plugin takes: shared D3D11 textures on the adapter it draws
with, CPU BGRA frames otherwise (`--cpu` forces them, `--no-hw` decodes in
software); its summary also counts the frames of each kind. CMake
downloads GLFW 3.4 there (hash-pinned; `-DLYCHEE_FETCH_GLFW=OFF` skips the
example). On Linux (`main_linux.cc`; needs `libglfw3-dev`, `libegl-dev` and
`libgles-dev`) it draws with OpenGL ES 3 on EGL, as the Flutter plugin
does: CPU RGBA frames uploaded into a texture, and VAAPI dmabufs imported
as EGLImages and converted from YUV by a shader. There `--size WxH` sets
the window's client size, and `--snapshot SEC:FILE` (repeatable) reads
back what the window drew for the first frame at or after SEC seconds,
writes it as a binary PPM (P6, top row first) and prints `snapshot FILE
pts=US size=WxH video=X,Y,W,H` (the frame's pts on the file's timeline,
and where the picture is in the PPM).

`tool/regression.sh` generates a synthetic media set and runs the CLI over it
(`TSAN=1` for a ThreadSanitizer build, `ASAN=1` for AddressSanitizer and
UndefinedBehaviorSanitizer, `COVERAGE=1` for a line coverage report).
`tool/format.sh` runs clang-format on the lines a branch changed. `tool/fuzz.sh` fuzzes the
FFmpeg-free parsers (MP4, Matroska, MPEG-TS, HLS) with libFuzzer under
ASan and UBSan (`FUZZ_TARGET`, `FUZZ_SECONDS`, `JOBS`); on macOS it
needs Homebrew's LLVM (`brew install llvm`, a development tool only),
since Apple's clang has no libFuzzer runtime, and clang on Linux. Inputs
they once crashed on live in `core/fuzz/regressions/TARGET`, which the
unit tests replay. `tool/changes.sh` names the areas a branch touches, which
decide the checks it needs and the CI jobs that run. Contributors and
agents: see
[AGENTS.md](AGENTS.md) and [docs/roadmap.md](docs/roadmap.md).

### Checking frames by pixels

`tool/frame_check.py` (Python's standard library and the `ffmpeg` and
`ffprobe` tools) checks a snapshot against the media: it decodes the frame
at the snapshot's pts with FFmpeg, upright, scales it to the video
rectangle, and compares. It prints PASS or FAIL with the numbers and on
FAIL writes `FILE.diff.ppm` next to the snapshot (the difference, four
times brighter).

```sh
lychee_cli --virtual-time --seconds 1.2 --frame-ppm 1:/tmp/f.ppm video.mp4 |
  python3 -I tool/frame_check.py video.mp4 --log -
lychee_glfw --seconds 2 --size 800x600 --snapshot 1.5:/tmp/w.ppm video.mp4 |
  python3 -I tool/frame_check.py video.mp4 --log -
python3 -I tool/frame_check.py video.mp4 /tmp/w.ppm --pts 1033333 \
  --video 0,150,1600,900 --self-test
```

The reference follows the library's conventions: a stream without a color
matrix is BT.709 above 576 lines and BT.601 below, and HDR is tone-mapped
to SDR BT.709 by swscale. A snapshot passes with PSNR at least 28 dB, a
mean absolute error at most 4 (of 255), the worst 32x32 tile's at most 40,
and, in a window, black outside the video rectangle. On the regression
media (2026-10-26) the right frame gives 30.4 dB or more, an error of 0.6
to 1.8 and a worst tile of 5 to 15, through every frame path on macOS
and Windows, at 1:1 and fitted into windows down to a quarter of its
size; the 640x360 rendition through swscale gives 29.8 dB (2026-10-27).
`--self-test` makes wrong frames from the snapshot and checks that each
fails; there they gave:

| wrong frame | PSNR | error | worst tile |
| --- | --- | --- | --- |
| the frame before or after | 19-21 dB | 3.4-12.3 | 68-217 |
| turned 180 degrees | 0.5-0.8 dB | 228-238 | 245-250 |
| turned 90 degrees, or a window rectangle of another shape | size or aspect differs | | |
| 2% too large | 17-18 dB | 6.7-15.3 | 54-214 |
| 4 pixels to the left (window) | 17.6-20.2 dB | 4.5-7.8 | 47-92 |
| black | 3.4 dB | 127 | 183 |
| BT.601 read as BT.709 | 23.4-23.9 dB | 9.2-9.8 | 16-20 |

The frame before or after differs only where the test pattern moves, which
the worst tile catches. Not covered: VideoToolbox maps bright HDR colors
differently from swscale (`hdr_pq_bars.mp4` gives 27.9 dB), and fitting a
frame into a window far smaller than it (GPU bilinear sampling without
mipmaps aliases) costs PSNR. The reference is decoded from the start of
the file, so late snapshots of long files take longer.

## Apple frameworks

The Flutter plugin's iOS/macOS pod and the Swift package both link prebuilt
xcframeworks from `flutter/darwin/Frameworks`: `lychee.xcframework` (the
core, a dynamic framework carrying only `lychee/lychee.h`), an LGPL-only
FFmpeg, and libass (with FreeType and HarfBuzz linked in; system fonts
through Core Text) with FriBidi, for styled ASS. Build them once, and
rebuild `lychee.xcframework` after changing `core/` (FFmpeg 9 source tree
as `SRC`; the libass, FreeType, HarfBuzz and FriBidi trees that
`tool/release.sh sources` downloads, which `tool/release.sh build apple
DIR` also unpacks and builds; libass needs `meson`):

```sh
for p in macos-arm64 macos-x86_64 ios-arm64 ios-sim-arm64 ios-sim-x86_64; do
  tool/build_ffmpeg.sh $p SRC
  tool/build_libass.sh $p LIBASS FREETYPE HARFBUZZ FRIBIDI
done
tool/package_ffmpeg_apple.sh   # FFmpeg, libass and FriBidi xcframeworks
tool/build_apple.sh            # lychee.xcframework
```

Each xcframework has a universal macOS slice, an iOS device slice and one
simulator slice with both arm64 and x86_64. Leave out platforms you do not
need; the last two scripts package the ones that were built. The x86_64
macOS FFmpeg and libass use assembly only if `nasm` is installed.

`NO_FFMPEG=1 tool/build_apple.sh` builds the core without FFmpeg and
libass as `lychee_noffmpeg.xcframework` (still `lychee.framework` inside;
MP4, Matroska/WebM, MPEG-TS and HLS through the core's own demuxers,
VideoToolbox and AudioToolbox decoders), which needs none of the other
frameworks. The Flutter plugin does not use it.

## Swift package

`sdk/apple` is a Swift package (`LycheePlayer`, macOS 11 / iOS 16) for apps
without Flutter. Add it as a local package; it takes the xcframeworks above
as binary targets. Its default trait, `FFmpeg`, links the core with FFmpeg
and libass; the `NoFFmpeg` trait takes `lychee_noffmpeg.xcframework`
instead, e.g. `.package(path: "sdk/apple", traits: ["NoFFmpeg"])` (Xcode:
the package reference's traits), and `swift test --traits NoFFmpeg` tests
it.

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
emulators) and libass (`tool/build_libass.sh android-arm64-v8a LIBASS
FREETYPE HARFBUZZ FRIBIDI FONTS`, FONTS the Liberation Fonts TTF release:
Android gives libass no system fonts, so the core embeds Liberation Sans,
metrically compatible with the Arial most scripts name, and falls back to
the system's Noto Sans CJK for what it lacks), and Kotlin classes over it. The core is also a prefab module
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

The plugin lives in `flutter/`. On iOS and macOS it builds with CocoaPods
(`darwin/lychee_player.podspec`) or, with Flutter's Swift Package Manager
integration on, as a Swift package (`darwin/lychee_player/Package.swift`);
both take the Apple frameworks above. The Swift package pulls frames
through `sdk/apple`'s `LycheeFrameDriver` (`tool/release.sh` copies its
sources into the published package); the pod cannot depend on a Swift
package and keeps a port of it.

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
core has libass (macOS, iOS and Android; not yet Windows and Linux),
arrive as pictures in `player.value.subtitlePictures`, which `LycheeVideo`
draws over the video.

The example app (`flutter/example`) turns off the macOS App Sandbox so it can
open arbitrary paths. It plays `--dart-define=LYCHEE_URL=...` on start.

Outside this repository (from pub.dev, or a git dependency without local
builds), the plugin takes the Apple frameworks and the Android AAR from the
GitHub release that `flutter/prebuilt.json` pins, downloaded into
`~/.cache/lychee_player` (SwiftPM's own cache with Swift Package
Manager) and checked against their SHA-256s; `LYCHEE_PREBUILT_URL` names
a mirror of the release URL (an https one for SwiftPM). Inside it, local
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
- **Linux**: `flutter build linux`. The plugin downloads the FFmpeg that
  `flutter/prebuilt.json` pins (`ffmpeg-linux-x86_64.tar.xz` or
  `ffmpeg-linux-arm64.tar.xz`, the release's decode-only LGPL 2.1 build,
  8 MB of libraries; BtbN's LGPLv3 one while the pinned release has
  none) into
  `~/.cache/lychee_player` on first configure and bundles it next to
  `liblychee_player.so`; `LYCHEE_PREBUILT_URL` mirrors it and
  `LYCHEE_FFMPEG_ROOT` (CMake or environment) names a local one instead,
  e.g. from `tool/build_ffmpeg.sh linux-x86_64 SRC` or `linux-arm64`,
  on that architecture (needs `pkg-config`, `libva-dev`, `libdrm-dev`,
  `libgnutls28-dev`, `zlib1g-dev` and on x86_64 `nasm`; the release
  builds both on Ubuntu 22.04, the oldest distribution it supports, for
  its glibc, on GitHub's x86_64 and arm64 runners). Its libraries load the
  system's libva, libva-drm, libdrm, GnuTLS and zlib (`libva2`,
  `libva-drm2`, `libdrm2`, `libgnutls30`, `zlib1g`). Apps need
  `libgtk-3` and `libpulse0` at runtime. The core alone needs FFmpeg 8 or newer, which Ubuntu 22.04
  does not ship: point pkg-config at a prebuilt one
  (`PKG_CONFIG_PATH=<ffmpeg>/lib/pkgconfig`, `LD_LIBRARY_PATH=<ffmpeg>/lib`
  to run the CLI) and install `libpulse-dev`.

## Releases

A release is one run of `.github/workflows/release.yml`, after a
`## VERSION` section in `flutter/CHANGELOG.md`:

```sh
gh workflow run release.yml -f version=1.0.0
```

It builds the Apple xcframeworks, the Android AAR and the Windows and
Linux FFmpeg (on Ubuntu; Windows cross-compiled), pins them in
`flutter/prebuilt.json` and `Package.swift` (which lets SwiftPM take
`sdk/apple` by URL), builds a fresh app against the package pub would
publish, drafts the GitHub release with the assets and the sources of
FFmpeg, Mbed TLS, libass, FreeType, HarfBuzz and FriBidi (and the
Liberation fonts), and pushes the commit `Release VERSION` and the tag
`vVERSION`. The tag starts `publish.yml`, which publishes the release and
the package on pub.dev. Each step is a `tool/release.sh` command and runs
locally too:

```sh
tool/release.sh version 1.0.0
tool/release.sh build apple /tmp/sources && tool/release.sh pack apple /tmp/dist
tool/release.sh build android /tmp/sources && tool/release.sh pack android /tmp/dist
tool/release.sh build windows /tmp/sources && tool/release.sh pack windows /tmp/dist  # on Linux
tool/release.sh build linux /tmp/sources && tool/release.sh pack linux /tmp/dist  # on Ubuntu 22.04
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
line), and so are the Windows build (Schannel for https) and the Linux
build (GnuTLS, LGPL 2.1+, the system's); the Android build is LGPL 3+,
because it links mbedTLS (Apache-2.0) for https, and so is BtbN's Linux
download (until a release carries ours). Distributed apps
must ship an LGPL-only FFmpeg (no `--enable-gpl` / `--enable-nonfree`),
linked dynamically.

[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) has the notices an app must
ship with lychee_player (Chromium, FFmpeg, Mbed TLS, libass, FreeType,
HarfBuzz, FriBidi, Liberation Sans, and their license texts). FriBidi is
LGPL 2.1+ and stays its own dynamic library; libass (ISC), FreeType (FTL)
and HarfBuzz (MIT) are linked into one. The prebuilt artifacts carry it: `lychee.framework` and, on
Android, `META-INF/lychee/THIRD_PARTY_NOTICES` in the APK.

`tool/build_ffmpeg.sh` builds only what the core plays: MP4/MOV,
Matroska/WebM, MPEG-TS, HLS, FLV, Ogg and raw audio files; H.264, HEVC,
VP8, VP9 and AV1 (AV1 only through VideoToolbox: FFmpeg has no AV1
software decoder of its own), AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC-3,
E-AC-3 and PCM; SRT, WebVTT, mov_text, ASS, PGS, DVB and VobSub
subtitles; file, http(s)
and AES-128 HLS. Other formats need an entry in its lists.
