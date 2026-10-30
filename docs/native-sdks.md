# Native SDKs

lychee_player without Flutter: a Swift package for macOS and iOS, an
Android library, Rust crates, and the C ABI under all of them.

## Swift

`sdk/apple` is a Swift package (`LycheePlayer`, macOS 11 / iOS 16) for apps
without Flutter. Add it by URL; it takes the prebuilt core from the
GitHub release:

```swift
.package(url: "https://github.com/boyan01/lychee_player", from: "1.0.0-beta.3")
```

In this repository, `sdk/apple` is the same package as a local one, built
against the xcframeworks in `flutter/darwin/Frameworks`
([development.md](development.md#apple-frameworks)). Its default trait, `FFmpeg`, links the core with FFmpeg;
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

## Android

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

Both are on Maven Central. To try a local build instead, publish both
into `sdk/android/build/repo` and add that directory as a Maven
repository in the app's `settings.gradle.kts`:

```sh
sdk/android/gradlew -p sdk/android :lychee:publish :ffmpeg:publish
```

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

## Rust

The crates are not on crates.io yet; depend on them by git or path.

`rust/` is a Cargo workspace: `lychee-sys` holds the raw bindings
(generated from `lychee.h` by `tool/rust_bindings.sh` and checked in, so
no libclang is needed) and links `liblychee` as a shared library, and
`lychee` is the safe API over it. `lychee-sys` builds the core with CMake
(the tools from [development.md](development.md#building-the-core)); FFmpeg comes from pkg-config, or
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

## C ABI

`core/include/lychee/lychee.h` is the only public header, and every
binding above is a thin layer over it.

Video frames leave the library as native handles (`CVPixelBufferRef` on Apple
platforms) or as CPU RGBA rows elsewhere. The caller pulls them per vsync with
`lychee_player_has_new_frame()` / `lychee_player_acquire_frame()`, and the
library picks the frame that should be on screen at that time.

Besides files and URLs, the library plays a live input
(`lychee_player_open_live()`): the caller declares H.264/HEVC and AAC/ALAC
tracks and pushes the packets a sender (e.g. screen mirroring) sends,
with the sender's timestamps; the library presents them a fixed latency
after they arrive and adapts to jitter and the audio device's latency.

Native Windows callers can accept `LYCHEE_FRAME_D3D11_TEXTURE_HDR` to
keep HDR in a shared `DXGI_FORMAT_R10G10B10A2_UNORM` texture. It carries
full-range BT.2020 RGB with the PQ transfer function (ST 2084, 1.0 =
10000 nits); HLG is converted for a 1000-nit reference display. Present
it using `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Also accept the
existing `LYCHEE_FRAME_D3D11_TEXTURE` for SDR and a CPU frame type for
decoder fallback. Consumers that accept only the old BGRA type continue
to get HDR tone-mapped to SDR.
