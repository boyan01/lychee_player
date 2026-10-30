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
    backends/mp2t/    MPEG-TS demuxer: without FFmpeg, and with it on Apple
    backends/hls/     HLS demuxer (TS, fMP4): without FFmpeg, and with it
                      on Apple, where FFmpeg takes what these cannot play
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
(`brew install cmake ninja pkg-config ffmpeg`). On macOS and
iOS, `-DLYCHEE_WITH_FFMPEG=OFF` builds without FFmpeg: MP4 (fragmented
too), Matroska/WebM, MPEG-TS and HLS (TS and fMP4 segments, AES-128,
audio renditions, WebVTT and fMP4 WebVTT/IMSC subtitle renditions,
variants as video tracks), from files and http(s)
URLs, with
H.264/HEVC (VideoToolbox), AAC, ALAC, MP3, Opus, FLAC and AC-3
(AudioToolbox) and tx3g subtitles;
`NO_FFMPEG=1 tool/regression.sh` tests it. On Windows it plays the same
containers with H.264, HEVC, VP9 and AV1 (Media Foundation, on the GPU or
the CPU), AAC,
MP3, FLAC, ALAC, Opus and AC-3, and text subtitles; the Flutter plugin builds it so with
`LYCHEE_WITH_FFMPEG=OFF` in the environment of `flutter build windows`.
HEVC, VP9 and AV1 require the corresponding Microsoft Store Video
Extensions. Missing extensions leave that video track unavailable in
FFmpeg-free builds; builds with FFmpeg keep their decoder fallback.
Android's plays them with H.264/HEVC and AAC, MP3, Opus, FLAC, ALAC and AC-3
through MediaCodec. AC-3 requires an installed system decoder; when one
is missing, the audio track is disabled and video continues. Both read http(s) through the system (WinHTTP,
`HttpURLConnection`). IMSC text keeps paragraph/span timing and line breaks;
styles are dropped. IMSC image subtitles use timed PNG pictures in their
explicit regions, through the existing bitmap-subtitle API. Region extents
use pixels and match the PNG dimensions; origins use pixels, or percentages
when the root canvas has a pixel extent, as defined in
[IMSC 1.2 section 10](https://www.w3.org/TR/ttml-imsc1.2/#image-profile-constraints).
Both ISO 14496-30
`subs` auxiliary resources and relative/absolute file or HTTP(S) image
URIs are read; PNG dimensions, input and decoded pixels are bounded.
PNG decoding uses ImageIO, Windows Imaging Component, Android's system
decoders, and the system libpng on Linux. Android apps on API 26–29 need
the Java VM retained by `System.loadLibrary("lychee")`; API 30 and later
also have a native PNG decoder for programs without a VM.
This is basic image placement,
with no text styling or layout engine. HLS measures active network transfers on all three system
HTTP clients and adapts between compatible TS or fMP4 variants.

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
`--d3d11-hdr` (Windows: PQ / BT.2020 R10G10B10A2 textures for HDR,
BGRA for SDR and CPU fallback),
`--refresh HZ`, `--vsync-jitter US`, `--volume 0..1` (muted by default),
`--rate R [--rate-after SEC]`, `--loop`, `--open-after SEC URL` (reopen on
the same player), `--select TYPE:INDEX[@SEC] [--select-after SEC]` (TYPE
is `video`, `audio` or `subtitle`, INDEX a track index as the CLI lists
them; -1 turns the type off; `@SEC` makes that one SEC seconds in instead
of `--select-after`'s), `--virtual-time` (play on a simulated clock, as fast
as the media decodes, with audio to a null sink). `--live` pushes the
file's packets into a live input when a sender would send them, with
`--latency MS`, `--live-jitter MS`, `--live-stall SEC FOR`,
`--live-flush SEC` and `--live-gap SEC FOR` to simulate the network.
`--subtitle-ppm FILE` writes each subtitle with pictures (bitmap
subtitles) onto a grey canvas of the video's size.
`--frame-ppm SEC:FILE` (repeatable) writes the frame on screen SEC seconds
into the run, upright, as a binary PPM (CPU frames as they are,
`CVPixelBuffer`s through VideoToolbox to SDR BT.709, D3D11 textures through
a staging copy) and prints `snapshot FILE pts=US size=WxH`.
With `--d3d11-hdr`, the dump retains PQ samples normalized to bytes;
it is a data check, not an SDR preview. The CLI also prints the texture
format and the center pixel's raw 10-bit RGB values.

Native Windows callers can accept `LYCHEE_FRAME_D3D11_TEXTURE_HDR` to
keep HDR in a shared `DXGI_FORMAT_R10G10B10A2_UNORM` texture. It carries
full-range BT.2020 RGB with the PQ transfer function (ST 2084, 1.0 =
10000 nits); HLG is converted for a 1000-nit reference display. Present
it using `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Also accept the
existing `LYCHEE_FRAME_D3D11_TEXTURE` for SDR and a CPU frame type for
decoder fallback. Consumers that accept only the old BGRA type continue
to get HDR tone-mapped to SDR.

With GLFW installed (`brew install glfw`) the build also produces
`build/examples/glfw/lychee_glfw path/to/video.mp4`: a window that draws the
frames with Metal. Space pauses, left/right seek 5 s, escape quits.
`--seconds N` quits after N seconds and `--key SEC:NAME` (`space`, `left`,
`right`, `escape`) presses a key at SEC seconds, for unattended runs; it
then prints a summary (decoder, frames rendered and dropped, position).
`--soak SEC URL` loops and, every SEC seconds, seeks twice to random
positions and then switches to the other file. `MINUTES=10 tool/soak.sh
-- build/examples/glfw/lychee_glfw --soak 3 OTHER_FILE FILE` checks the
harness's memory and thread counts while it does so.
`--size WxH` sets the window's client size (in points on macOS, where the
drawable is that times the display's scale), and `--snapshot SEC:FILE`
(repeatable; after any `--key` due at the same time) reads back what the
window drew once a frame is on screen and writes it as a binary PPM,
printing `snapshot FILE pts=US size=WxH video=X,Y,W,H` (the rectangle the
frame was fitted into). On
Windows the same program (`main_win.cc`) draws with Direct3D 11 the frames
the Flutter plugin takes: shared D3D11 textures on the adapter it draws
with, CPU BGRA frames otherwise (`--cpu` forces them, `--no-hw` decodes in
software); its summary also counts the frames of each kind and the
device losses (it makes a new device after a GPU reset), and `--adapter
SEC:N` moves it to the Nth DXGI adapter at SEC seconds, as a window moved
to a display on another GPU. Without `--adapter` it draws on the adapter
of the display the window is on, and follows the window to another one. CMake
downloads GLFW 3.5.1 there (hash-pinned; `-DLYCHEE_FETCH_GLFW=OFF` skips the
example). On Linux (`main_linux.cc`; needs `libglfw3-dev`, `libegl-dev` and
`libgles-dev`) it draws with OpenGL ES 3 on EGL, as the Flutter plugin
does: CPU RGBA frames uploaded into a texture, and VAAPI dmabufs imported
as EGLImages and converted from YUV by a shader. There `--size WxH` sets
the window's client size, and `--snapshot SEC:FILE` (repeatable) reads
back what the window drew for the first frame at or after SEC seconds,
writes it as a binary PPM (P6, top row first) and prints `snapshot FILE
pts=US size=WxH video=X,Y,W,H` (the frame's pts on the file's timeline,
and where the picture is in the PPM).

The Linux GLFW and Flutter shaders use dmabuf color metadata to map PQ
and HLG to SDR BT.709 using PQ-encoded IPT, the color model used by
swscale. A small hue table locates BT.709's most saturated color, then
colors outside that gamut converge towards it. SDR white is 203 nits; HLG
uses a 1000-nit reference display. The core
keeps the original P010 planes and color metadata for native HDR callers.

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

The Swift package's example takes the same `--seconds` and `--snapshot`
(and on macOS `--size WxH` for the video view, in points). It snapshots
what the window server composited: on macOS through ScreenCaptureKit,
which needs the Screen Recording permission of the app that launches it
(a terminal), and on iOS from the view hierarchy. Pausing for the
capture keeps the frame on screen while it runs. In the iOS simulator
pass the arguments to `simctl launch --console-pty`:

```sh
Example.app/Contents/MacOS/Example --seconds 3 --size 405x720 \
  --snapshot 1.5:/tmp/s.ppm video.mp4 | python3 -I tool/frame_check.py video.mp4 --log -
xcrun simctl launch --console-pty booted tech.soit.lychee.example --seconds 5 \
  --snapshot 3:/tmp/s.ppm video.mp4 | python3 -I tool/frame_check.py video.mp4 --log -
```

The Android SDK's example takes snapshots with its `snapshot` intent
extra (`SEC:NAME,...`, seconds after the first displayed frame): it pauses,
copies the `SurfaceView` with
`PixelCopy` (`LycheePlayerView.snapshot`), writes the PPM into its
external files directory and logs the line, then plays on.
An absolute `NAME` writes to that path instead. This permits app-private
files, readable with `adb exec-out run-as PACKAGE cat PATH`, when scoped
storage blocks adb from its external files.

```sh
adb shell am start -n tech.soit.lychee.example/.MainActivity \
  --es url http://127.0.0.1:8765/rotated.mp4 --ez hw false \
  --es snapshot 2:a.ppm,3.5:b.ppm
adb pull /sdcard/Android/data/tech.soit.lychee.example/files/. /tmp/snaps
adb logcat -d -s lychee-example:I | grep -o 'snapshot .*' |
  sed 's|/[^ ]*/files/|/tmp/snaps/|' |
  python3 -I tool/frame_check.py rotated.mp4 --log - --bilinear --matrix bt601
```

The emulator's compositor converts MediaCodec's YUV surfaces with BT.601
whatever their dataspace, for both hardware and system software decoding.
HD fails there by its matrix alone; `--matrix bt601` checks the rest
(frame, pts, rotation, scale). Omit it on a phone, which shows whether
that path keeps BT.709, and for FFmpeg's software RGB output.
`--bilinear` converts to RGB at native size, then samples the four source
pixels around each destination pixel center, as `PixelCopy` does when
resizing the surface into the bitmap. Without it, the reference keeps
swscale's downscaling low-pass filter. These differ on fine patterns at
large reductions: the API 34 emulator's 320x180 H.264 snapshots give
27.2-27.5 dB against the low-pass reference and over 35 dB against the
texture sampler (2026-10-29). Both modes keep the same thresholds and
reject the wrong PTS, matrix, rotation and scale.

The reference follows the library's conventions: a stream without a color
matrix is BT.709 above 576 lines and BT.601 below, and HDR is tone-mapped
to SDR BT.709 by swscale. HDR conversion stays at 16 bits until its final
SDR result is quantized: the core uses planar GBR(A), retaining alpha, and
the pixel reference converts at native size to RGB48 without
`full_chroma_int`, then resizes the SDR RGB. Direct RGB8
output and that flag can change flat HDR colors in FFmpeg 9.0.2. The
reference's SDR flags and all pixel thresholds stay the same.
`python3 -I tool/frame_check_test.py` checks the HDR reference against
independently generated in-gamut light. Unspecified source primaries and transfer use
BT.709 too. The FFmpeg-free Apple H.264/HEVC decoder gives container tags
priority over the parameter-set format's source tags; absent fields use those
defaults, rather than VideoToolbox's guessed SD profile.
A snapshot passes with PSNR at least 28 dB, a
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
frame into a window far smaller than it costs PSNR unless `--bilinear`
matches its texture sampling without mipmaps. The reference is decoded
from the start of the file, so late snapshots of long files take longer.

## Apple frameworks

The Flutter plugin's iOS/macOS pod and the Swift package both link prebuilt
xcframeworks from `flutter/darwin/Frameworks`: `lychee.xcframework` (the
core, a dynamic framework carrying only `lychee/lychee.h`) and an
LGPL-only FFmpeg. Build them once, and rebuild `lychee.xcframework` after
changing `core/` (FFmpeg 9 source tree as `SRC`; `tool/release.sh build
apple DIR` also downloads and builds it):

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

`NO_FFMPEG=1 tool/build_apple.sh` builds the core without FFmpeg as
`lychee_noffmpeg.xcframework` (still `lychee.framework` inside;
MP4, Matroska/WebM, MPEG-TS and HLS through the core's own demuxers,
VideoToolbox and AudioToolbox decoders), which needs none of the other
frameworks. The Flutter plugin does not use it.

## Swift package

`sdk/apple` is a Swift package (`LycheePlayer`, macOS 11 / iOS 16) for apps
without Flutter. Add it as a local package; it takes the xcframeworks above
as binary targets. Its default trait, `FFmpeg`, links the core with FFmpeg;
the `NoFFmpeg` trait takes `lychee_noffmpeg.xcframework`
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

The Swift example accepts the same `--soak SEC URL` and `--seconds N`
arguments as the GLFW harness. Both its macOS app and iOS simulator app
repeat seeks and file switches through their display-link frame driver.

For the iOS simulator use `-destination 'generic/platform=iOS Simulator'`,
`xcrun simctl install booted .../Debug-iphonesimulator/Example.app` and
`SIMCTL_CHILD_LYCHEE_URL=... xcrun simctl launch booted tech.soit.lychee.example`;
the simulator reads `/tmp` of the Mac.

## Android library

`sdk/android` builds two complementary AARs (minSdk 26), both with
`armeabi-v7a`, `arm64-v8a` and `x86_64`:

- `io.github.boyan01.lychee:lychee`: the FFmpeg-free core as `liblychee.so`,
  Kotlin's `LycheePlayer`, `LycheeFrameDriver` and `LycheePlayerView`.
  MP4, Matroska/WebM, MPEG-TS and HLS use the own demuxers, and Android's
  MediaCodec supplies hardware and software audio/video decoders. The core
  is also a Prefab module (`lychee`, header `lychee/lychee.h`) for C ABI callers.
- `io.github.boyan01.lychee:lychee-ffmpeg`: the optional FFmpeg backend and its
  LGPL shared libraries. It depends on the matching base AAR and contains
  no second player core. Build FFmpeg first with
  `tool/build_ffmpeg.sh android-arm64-v8a SRC`, `android-armeabi-v7a`
  and `android-x86_64` for the other ABIs.

Publish both into `sdk/android/build/repo` for local consumption:

```sh
sdk/android/gradlew -p sdk/android :lychee:publish :ffmpeg:publish
```

Until the AARs are published on Maven Central, add the local repository
in the consuming app's `settings.gradle.kts`:

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("/path/to/lychee_player/sdk/android/build/repo") }
    }
}
```

The base AAR builds without any FFmpeg installation. Native apps can use
it alone. Add the optional dependency and explicitly load it to enable
FFmpeg (the two coordinates must have the same version):

```kotlin
dependencies {
    implementation("io.github.boyan01.lychee:lychee:$lycheeVersion")
    implementation("io.github.boyan01.lychee:lychee-ffmpeg:$lycheeVersion") // Optional.
}

// Application.onCreate(), before any player (including C ABI/Flutter players).
LycheeFFmpeg.load()
```

Repeated loads are safe, including after players exist. A first load after
creating a player throws `IllegalStateException`, as does a missing native
extension or a mismatched build. The loader checks a private fingerprint
of the core sources, SDK version and toolchain before exchanging internal
objects. Use both AARs from the same SDK build. A successful load stays
active for the process lifetime; it does not force software video decoding.
With it, MediaCodec hardware video remains first, with FFmpeg decoders as
fallback; audio and subtitles use FFmpeg, as in the previous bundled build.
Without `load()`, even an app that includes the optional AAR keeps using
the system backend.

HTTPS uses HttpURLConnection and AES-128 HLS uses javax.crypto in the
base. With the extension, HLS that needs FFmpeg's demuxer also uses the
platform HTTP client; its AES-128 segments are decrypted in the backend
with `av_aes`. FFmpeg handles cleartext HTTP refused by the app's network
security config. That fallback probes redirects with HEAD (GET with a
one-byte Range if HEAD is unsupported), follows at most five redirects,
and opens an HTTPS destination through HttpURLConnection. A host whose URL
did not redirect is not probed again until FFmpeg fails on one of its
URLs; FFmpeg has no TLS on Android. The native CLI run from `adb shell`
has no Java VM and cannot open HTTPS.

Either way http(s) goes through Java's `HttpURLConnection`, so the app's
network security config applies: an app that plays plain http needs
`android:usesCleartextTraffic` (or a config that allows the hosts). With
FFmpeg, cleartext the config refuses falls back to FFmpeg's own http.

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

The example app (`sdk/android/example`, `:example:assembleDebug`) defaults
to the base package. Build with `-Plychee.example.ffmpeg=true` to include
the extension, then pass `--es ffmpeg load` to enable it. The harness also
accepts `twice`, `concurrent` and `late` to check loading behavior.
`tool/android_extension_check.sh` consumes the two local Maven publications
in minified apps, checks frame snapshots with both backends, and checks
loading failures (including a deliberately missing library).

The example plays the
`url` intent extra, by default `h264_aac.mp4` from `tool/serve_media.py`
through `adb reverse tcp:8765 tcp:8765`; `--ez hw false` decodes in
software. `--es soak 3 --es soak_url OTHER_URL` repeats seeks and file
switches every three seconds, and `--es seconds 600` closes the activity
after ten minutes with a `summary:` line in logcat:

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
in an app bundle). On Windows build in a Visual Studio developer prompt
with `LYCHEE_FFMPEG_ROOT` set (`tool/build_ffmpeg.sh windows-x86_64`, or
the release's `ffmpeg-windows-x86_64.zip`); `LYCHEE_LIB_DIR` there needs
`lychee.lib` beside `lychee.dll`. Windows finds DLLs next to the binary or
on `PATH`: put FFmpeg's `bin/` (and a `LYCHEE_LIB_DIR` outside `target/`)
there for `cargo test`, and ship `lychee.dll` and the FFmpeg DLLs next to
the binary. On Linux `LYCHEE_LIB_DIR` takes the core's own CMake build
(`-DBUILD_SHARED_LIBS=ON`, `build/core/liblychee.so`), which finds FFmpeg
through its runpath; the loader finds `liblychee.so` itself only inside
`target/`, so set `LD_LIBRARY_PATH` to a `LYCHEE_LIB_DIR` elsewhere for
`cargo test`. The test reads the media from `MEDIA_DIR` if set.

For iOS, point `LYCHEE_LIB_DIR` at a slice of `lychee.xcframework` (or
`lychee_noffmpeg.xcframework`, which needs no FFmpeg), from
`tool/build_apple.sh` or a release, and build for `aarch64-apple-ios` or
`aarch64-apple-ios-sim`; `lychee-sys` links the slice's `lychee.framework`.
An app embeds the frameworks (lychee and, with FFmpeg, its five) under
`Frameworks/`. The tests run on a booted simulator without an app:

```sh
F=$PWD/flutter/darwin/Frameworks S=ios-arm64_x86_64-simulator
LYCHEE_LIB_DIR=$F/lychee_noffmpeg.xcframework/$S \
  RUSTFLAGS="-C link-arg=-Wl,-rpath,$F/lychee_noffmpeg.xcframework/$S" \
  cargo build --manifest-path rust/Cargo.toml --target aarch64-apple-ios-sim --tests
xcrun simctl spawn booted rust/target/aarch64-apple-ios-sim/debug/deps/play-HASH
```

Without FFmpeg, `subtitle_pictures` fails: PGS needs FFmpeg's decoder.

For Android, link the AAR's libraries (`jni/ABI` in
`sdk/android/build/repo/.../lychee-VERSION.aar`) with the NDK's clang as
the linker; an app loads `liblychee.so` with them (`System.loadLibrary`,
which also gives the core the Java VM its http client needs; a program
run from `adb shell` reads http through FFmpeg):

```sh
rustup target add x86_64-linux-android
CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/x86_64-linux-android26-clang \
  LYCHEE_LIB_DIR=/tmp/aar/jni/x86_64 cargo test --target x86_64-linux-android --no-run
```

The tests read their media from `MEDIA_DIR` (default
`/tmp/lychee-media`): push the test binary, the libraries and the media
to `/data/local/tmp`, and run it there with `LD_LIBRARY_PATH` set.

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
draws them; ASS arrives as its plain text too. Bitmap subtitles (PGS,
DVB, VobSub) arrive as pictures in `player.value.subtitlePictures`, which
`LycheeVideo` draws over the video.

The example app (`flutter/example`) turns off the macOS App Sandbox so it can
open arbitrary paths. It plays `--dart-define=LYCHEE_URL=...` on start.
On iOS and Android, which pass no arguments, `--dart-define=LYCHEE_SECONDS=5`
does what `--seconds 5` does on the desktop.

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


### Checking audio for clicks

`lychee_cli --audio-dump FILE` (with `--virtual-time` or `--null-audio`,
and `--volume 1`) records what the silent audio device output, mixed on
the clock's timeline with silence where it played nothing, as a 48 kHz
float WAV, until after the player is destroyed (a stop fades out too).
`tool/audio_check.py` finds clicks in it: steps in the
waveform, which a sine (the test media's audio) never has. A seek, a
pause (`--pause SEC FOR`) and a rate change fade the audio out and in
over 10 ms, so they make none; `--max-gap MS` also fails on silence
inside the recording, for a loop, whose passes must join without a gap.

```sh
lychee_cli --virtual-time --volume 1 --seconds 4 --seek 5.2 --seek-after 1 \
  --audio-dump /tmp/a.wav video.mp4
python3 -I tool/audio_check.py /tmp/a.wav
```
### Per platform

- **macOS and iOS**: the xcframeworks above; macOS 11 and iOS 16 or newer.
  The current SDK's libc++ supports nothing older on macOS, and the core
  converts frames with `VTPixelTransferSession` (iOS 16).
- **Android**: the AAR from the Android library above, published into
  `sdk/android/build/repo`, where the plugin looks for it first; rerun the
  publish after changing `core/`. The base AAR has all three supported
  ABIs and needs minSdk 26 (AAudio); FFmpeg is optional. Native Android
  hosts can add `lychee-ffmpeg` to their app and call `LycheeFFmpeg.load()`
  in `Application.onCreate()` before Flutter creates any players.
- **Windows**: `flutter build windows`. The plugin downloads the FFmpeg
  that `flutter/prebuilt.json` pins (`ffmpeg-windows-x86_64.zip`, the
  release's decode-only LGPL 2.1 build, 8 MB of DLLs) at configure time and ships its DLLs
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
  8 MB of libraries) into
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
  `libgtk-3`, `libpulse0` and `libpng16-16` at runtime. The core alone needs FFmpeg 8 or newer, which Ubuntu 22.04
  does not ship: point pkg-config at a prebuilt one
  (`PKG_CONFIG_PATH=<ffmpeg>/lib/pkgconfig`, `LD_LIBRARY_PATH=<ffmpeg>/lib`
  to run the CLI) and install `libpulse-dev` and `libpng-dev`.

## Releases

A release is one run of `.github/workflows/release.yml`, after a
`## VERSION` section in `flutter/CHANGELOG.md`:

```sh
gh workflow run release.yml -f version=1.0.0
```

It builds the Apple xcframeworks, both Android AARs and the Windows and
Linux FFmpeg (on Ubuntu; Windows cross-compiled), pins them in
`flutter/prebuilt.json` and `Package.swift` (which lets SwiftPM take
`sdk/apple` by URL), builds a fresh app against the package pub would
publish (macOS, iOS and Android on macOS, Windows on Windows), attests
the assets' build provenance and an SPDX SBOM of them, drafts the GitHub
release with the assets, the SBOM and the sources of FFmpeg and dav1d,
and pushes the commit `Release VERSION` and the tag
`vVERSION`. The tag starts `publish.yml`, which publishes the release and
the package on pub.dev, then both Android AARs on Maven Central.
Each step is a `tool/release.sh` command and runs
locally too:

```sh
tool/release.sh version 1.0.0
tool/release.sh build apple /tmp/sources && tool/release.sh pack apple /tmp/dist
tool/release.sh build android /tmp/sources && tool/release.sh pack android /tmp/dist
tool/release.sh build windows /tmp/sources && tool/release.sh pack windows /tmp/dist  # on Linux
tool/release.sh build linux /tmp/sources && tool/release.sh pack linux /tmp/dist  # on Ubuntu 22.04
tool/release.sh sources /tmp/dist && tool/release.sh pin /tmp/dist
tool/release.sh check /tmp/dist   # then git checkout the pins; in Git Bash on Windows it builds a Windows app, on Linux a Linux app
tool/release.sh sbom /tmp/dist    # lychee_player-VERSION.spdx.json
```

Anyone can check an asset against the workflow that built it:
`gh attestation verify ASSET --repo boyan01/lychee_player` (provenance;
`--predicate-type https://spdx.dev/Document/v2.3` for the SBOM).

Once per repository: a deploy key with write access whose private key is
the secret `RELEASE_DEPLOY_KEY` (the workflow's own token cannot start
`publish.yml`), a GitHub environment `pub.dev`, and automated publishing
on the package's pub.dev Admin page (repository `boyan01/lychee_player`,
tag pattern `v{{version}}`, environment `pub.dev`).

The Android publications are automatically sent to Maven Central after
GitHub and pub.dev publication succeeds. Both use the
`io.github.boyan01.lychee` group under the GitHub-verified
`io.github.boyan01` namespace. No custom domain or DNS verification is
needed. Maven coordinates do not change Kotlin package names: imports
remain `tech.soit.lychee.*`.

The Gradle `publish` tasks write only to the local repository. Each
publication includes its AAR, POM, Kotlin/native sources and generated
Dokka API documentation. `tool/release.sh pack android` includes all
eight files, and `pin` records their SHA-256 hashes. `Publish` calls the
reusable `Maven Central` workflow, which downloads those release assets, checks
their hashes, signs them and submits both AARs in one deployment.
It waits for all platform checks and GitHub/pub.dev publication, so a
failed platform check cannot publish an incomplete release to Central.
Normal releases need no second dispatch.
Create a new release containing these publications first; older tags
whose assets use the previous coordinates or omit sources/documentation
cannot be used by this workflow.

Configure four repository secrets before running that workflow:

| Secret | Value |
| --- | --- |
| `CENTRAL_USERNAME` | The username from a Central Portal user token |
| `CENTRAL_PASSWORD` | The password from the same token, not the account password |
| `MAVEN_SIGNING_KEY` | ASCII-armored private signing key, including its header/footer |
| `MAVEN_SIGNING_PASSWORD` | The key's passphrase |

Keep a protected backup of the signing key and passphrase outside the
checkout. Publish only its public key to a
[Central-supported keyserver](https://central.sonatype.org/publish/requirements/gpg/#distributing-your-public-key).
The token needs publishing access to the verified namespace. Neither
credential belongs in Gradle properties committed to Git.

For a local check, set `MAVEN_SIGNING_KEY` and `MAVEN_SIGNING_PASSWORD`
in the process environment, then run:

```sh
python3 -I tool/maven_central_test.py
python3 -I tool/maven_central.py bundle /tmp/dist --version VERSION --output /tmp/central-bundle.zip
python3 -I tool/maven_central.py check /tmp/central-bundle.zip --version VERSION
```

Verification can use `MAVEN_PUBLIC_KEY` instead of the private key.
`--unsigned` on `bundle` and `check` is for local packaging tests only;
unsigned bundles cannot be uploaded. The helper checks both artifacts,
matching versions, all three Android ABIs, POM metadata, sources,
documentation, checksums and signatures.

Manual dispatch of `Maven Central` remains available to stage a deployment
or recover a failed Central publication independently. Its `publish` input
defaults to `false`: Central validates and stages it for inspection.
The normal release workflow explicitly sets it to `true` for automatic
publication. For a separately authorized
local upload, `tool/maven_central.py upload /tmp/central-bundle.zip
--version VERSION` has the same staging default; `--publish` opts into
automatic publication. A released version is immutable on Central.

## License

MIT, see [LICENSE](LICENSE).

lychee_player uses [FFmpeg](https://ffmpeg.org) under the LGPL. The core's
own CMake build (the CLI, the GLFW example, `tool/regression.sh`) links the
FFmpeg found by pkg-config, which may be a GPL build (Homebrew's is); that
is for development only. The macOS and iOS builds from
`tool/build_ffmpeg.sh` are LGPL 2.1+ (the script checks configure's license
line), and so are the Windows build (Schannel for https) and the Linux
build (GnuTLS, LGPL 2.1+, the system's). Android is LGPL 2.1+ too:
FFmpeg provides cleartext HTTP, while HttpURLConnection handles HTTPS
and javax.crypto decrypts AES-128 HLS (FFmpeg's AES in its HLS
fallback). Distributed apps
must ship an LGPL-only FFmpeg (no `--enable-gpl` / `--enable-nonfree`),
linked dynamically.

[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) has the notices an app must
ship with lychee_player (Chromium, FFmpeg, dav1d, and their license
texts). The prebuilt artifacts carry it: `lychee.framework` and, on
Android, `META-INF/lychee/THIRD_PARTY_NOTICES` in the APK.

`tool/build_ffmpeg.sh` builds only what the core plays: MP4/MOV,
Matroska/WebM, MPEG-TS, HLS, FLV, Ogg and raw audio files; H.264, HEVC,
VP8, VP9 and AV1 (AV1 only through VideoToolbox: FFmpeg has no AV1
software decoder of its own), AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC-3,
E-AC-3 and PCM; SRT, WebVTT, mov_text, ASS, PGS, DVB and VobSub
subtitles; file, http(s)
and AES-128 HLS. Other formats need an entry in its lists.
