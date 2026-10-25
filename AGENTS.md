# AGENTS.md

lychee_player is a media player built as a reusable C++ library with a C ABI
(`core/`), plus a Flutter plugin that is one thin consumer of it. README.md
has the layout and build commands.

Before starting a feature or a new platform, read `docs/roadmap.md`: it lists
what is unfinished, known defects, and the done criteria for each item. Update
it in the same change when you finish, start, or discover an item.

## Design rules

These are decisions, not accidents. Keep them unless the user changes them.

- **The C ABI is the product.** `core/include/lychee/lychee.h` is the only
  public header. Everything crossing it is a plain C type: opaque handles,
  structs that start with `struct_size` so they can grow, callbacks with
  plain integer arguments (so Dart `NativeCallable.listener` can bind them).
  The core never depends on Flutter or any UI toolkit.
- **FFmpeg is an optional backend.** FFmpeg types and headers appear only
  under `core/src/backends/ffmpeg/`. `media/` and `pipeline/` use the
  library's own types (`EncodedPacket`, `VideoConfig`, `VideoFrame`, ...).
  Backends are chosen at build time in `media/factory.cc`.
- **Frames leave as native handles** (`CVPixelBufferRef` today; D3D11
  textures, dmabuf, AHardwareBuffer later). The library never renders. The
  caller pulls frames per vsync with `has_new_frame` / `acquire_frame`, in
  the `lychee_now_us()` timebase; the library picks the frame for that time.
- **License hygiene.** The project is MIT. FFmpeg must stay an LGPL-only
  build (no `--enable-gpl`, no `--enable-nonfree`), linked dynamically.
  Write code from your own understanding of ffplay or mpv rather than copying
  it: both are (L)GPL. Code ported from Chromium is BSD-3 and keeps its
  copyright header. Code from flutter-airplay
  (`~/workspace/github/flutter-airplay`) is the same author's and may be
  copied here under MIT (drop its GPL-3.0 SPDX line), except anything that
  came from UxPlay (its receiver and protocol code), which stays GPL.
- **Name the borrowing.** When porting a Chromium algorithm, say which file it
  comes from in a comment, so later readers can compare.

## Concurrency

The pipeline is multithreaded; most bugs here are lifetime and ordering bugs.

- Threads: the control thread (`Player::Run`: open, seeks, state machine,
  events), the read thread (`Player::ReadLoop`: only `Demuxer::Read`, so a
  stalled network read blocks nothing else), one decode thread per renderer, the CoreAudio render thread, and whatever
  thread the caller pulls frames on.
- Lock order is player → renderer → clock, and audio renderer → clock.
  Renderers never call back into the player.
- A seek interrupts the read in flight (`Demuxer::Interrupt`) and parks the
  read thread while the control thread seeks the demuxer.
- A seek bumps a **generation**. Packets and frames carry the generation they
  belong to; anything from an older generation is dropped, and a decoder is
  flushed when it first sees a newer one.
- Stopping a renderer requires aborting its `PacketQueue` first, or the
  decode thread stays blocked in `Pop`.
- A track switch, a new video device and a hardware decoder failing midway
  (a GPU reset) all replace renderers the same way: `ChangeTracks` on the
  control thread, then a resync seek to the clock's position. Video then
  skips frames up to the end of the one on screen, since callers pull
  frames for display times ahead of the clock.
- Live input (`lychee_player_open_live`): caller threads push packets into
  `LiveDemuxer`, which the read thread reads like any demuxer. Packets
  carry deadlines in the `NowMicros()` timebase, set at push time by
  `LiveTimeline` (ported from flutter-airplay); the clock runs at rate 1
  from the open and is never driven by audio, and seeks and rate changes
  are ignored. A track change or flush holds the stream in the demuxer
  until the control thread has parked the read thread, replaced or
  flushed the renderers under a new generation, and released it, so no
  packet reaches the old renderer and none is lost.
- Flutter teardown order matters: `disposeTexture` (stops the display link,
  nulls the player under the texture's lock) → `lychee_player_destroy`
  (joins all threads, so no more callbacks) → `NativeCallable.close()`.

## Verifying a change

`tool/regression.sh` builds the core, runs the unit tests in `core/test`,
and plays a fixed media matrix through `examples/cli` (headless, simulated
60 Hz vsync, muted). It prints PASS/FAIL per case; `NO_HW=1` decodes in
software, as on CI. Cases run in parallel (`JOBS`, default half the cores);
`ONLY='seek-* rate-*'` runs just the matching ones.

Most cases run on a virtual clock (`lychee_cli --virtual-time`): the CLI
advances `lychee_now_us()` one vsync at a time, but only once the pipeline
has decoded enough to play it (`lychee_testing_player_settled`), and audio
goes to `NullAudioSink`, which renders when the clock moves. Media plays as
fast as it decodes, and drift, drops and glitches do not depend on machine
load; `stalls=N` counts vsyncs the pipeline never caught up with. The hooks
live in `core/src/api/testing.h`, not in the public ABI. Cases that need
real time (a slow or stalled server, and the `realtime-*` cases that check
CoreAudio timing) set `realtime=1`.

While iterating, build and run the unit tests plus the cases your change
touches (`ONLY=...`, seconds); run the full matrices once, when done. CI runs
`NO_HW=1` plain and TSan on every push, so leave those to it unless you
changed `pipeline/`, threading or teardown. Do not run the `realtime=1`
cases while a Flutter or Gradle build loads the machine: their drift and
glitch checks fail for reasons unrelated to your change. Done means:

- `tool/regression.sh` passes with zero build warnings.
- For changes to `pipeline/`, threading, or teardown: `TSAN=1
  tool/regression.sh` also passes (it fails on any ThreadSanitizer report).
- For plugin or Dart changes: `flutter analyze` in `flutter/` is clean, and
  you have run the example (`cd flutter/example && flutter build macos --debug
  --dart-define=LYCHEE_URL=/tmp/lychee-media/h264_aac.mp4`, launch the app
  binary) and looked at a window screenshot. Say in your report what you
  did not see, e.g. audio you could not hear.
- For changes to player lifetime or teardown (Dart, plugin, or core):
  `cd flutter/example && flutter test integration_test -d macos` also
  passes. It switches files repeatedly and checks that exactly one player's
  threads remain. On Android it runs the same way (`-d emulator-5554`), but
  `flutter test` reinstalls the app, so serve the media instead:
  `python3 tool/serve_media.py /tmp/lychee-media 8765` and
  `--dart-define=LYCHEE_MEDIA=http://10.0.2.2:8765`.
- For plugin changes on other platforms: iOS on a simulator (`flutter build
  ios --simulator`, `xcrun simctl install/launch`, `xcrun simctl io
  screenshot`), Android on an emulator (`flutter build apk --debug`, `adb
  install`, `adb exec-out screencap`), Windows and Linux with the
  `flutter-remote-test` skill (`remote-test ... --project flutter/example`).
  Look at the screenshot: the stats line should show frames rendering at
  the video's rate.
- For changes to the Swift package (`sdk/apple`): `swift test` there
  (plays through the frame driver and checks the player's lifetime), and
  build its example
  (`sdk/apple/Example/Example.xcodeproj`, README has the commands) for
  macOS and the iOS simulator with zero warnings, run both, and look at
  the screenshots, including `rotated.mp4` when the view's layout changed.
  `LycheeFrameDriver` is a port of the plugin's `LycheeTexture`: fix both
  until the plugin uses the SDK (roadmap, Distribution 4.2).
- For changes to the Android SDK (`sdk/android`): build it and its example
  (`sdk/android/gradlew -p sdk/android :lychee:publish
  :example:assembleDebug`, zero warnings), install the example on an
  emulator or a phone and look at screenshots of `h264_aac.mp4` and
  `rotated.mp4`, each with hardware decoding and without (the `url` and
  `hw` intent extras). The Flutter plugin uses the same
  `LycheeFrameDriver`, so also run the Flutter example on Android.
- For changes to the Rust crates (`rust/`) or to `lychee.h`: `cargo test`,
  `cargo clippy --all-targets` and `cargo doc --no-deps` in `rust/` with
  zero warnings (the test plays `h264_aac.mp4` and checks the player's
  lifetime), and `cargo run --example play -- FILE frame.ppm`; look at the
  frame.
- For changes to what ends up on screen (frame selection, conversion,
  rotation): run `build/examples/glfw/lychee_glfw` on a test file and look at
  a window screenshot. It has no Flutter in between, so it also tells core
  bugs from texture bridge bugs.
- A new behaviour gets a case in `tool/regression.sh` when the CLI can
  observe it, and a unit test in `core/test` when it is a pure algorithm
  (clock, queue, frame selection, vsync estimation). The tests use the tiny
  harness in `core/test/test.h`; there is no test framework dependency.

## Gotchas

- The iOS/macOS pod (`flutter/darwin`) and the Swift package (`sdk/apple`)
  do not compile the core: they link
  `flutter/darwin/Frameworks/lychee.xcframework`, built by
  `tool/build_apple.sh`. After changing anything under `core/`, rerun it
  before building either example on macOS or iOS, or the example runs
  the old core. They may use only `lychee/lychee.h`; the framework carries
  no other header (and a module map, so Swift can `import lychee`).
- The Rust bindings (`rust/lychee-sys/src/bindings.rs`) are generated and
  checked in. After changing `lychee.h`, rerun `tool/rust_bindings.sh`
  (`cargo install bindgen-cli`) and wrap the change in the `lychee` crate.
- Xcode 27 builds apps for macOS 12 and later only, so the Swift example
  targets macOS 12 while the package and the frameworks say 11.
- `CALayer.contents` set to a `CVPixelBuffer` shows nothing in the iOS
  simulator; `LycheePlayerView` sets the buffer's IOSurface and keeps the
  buffer until the next frame.
- The Flutter plugin never links the system FFmpeg. Build the LGPL one once
  per platform with `tool/build_ffmpeg.sh PLATFORM <ffmpeg source>` into
  `third_party/ffmpeg/` (`macos-arm64` and `macos-x86_64`; `ios-sim-arm64`
  and `ios-sim-x86_64` for the simulator, `ios-arm64` for devices;
  `android-arm64-v8a`), then `tool/package_ffmpeg_apple.sh` and
  `tool/build_apple.sh` for Apple; the pod vendors the result from
  `flutter/darwin/Frameworks`. On Android the SDK's AAR (`sdk/android`)
  links `third_party/ffmpeg/android-ABI` and carries it. Use the FFmpeg major version
  Homebrew has: the core follows the current FFmpeg API (e.g.
  `sws_free_context`). After a new FFmpeg build, rerun all of them and
  `pod install` in `flutter/example/macos` and `flutter/example/ios`.
- The iOS simulator slice must hold both arm64 and x86_64: Flutter builds the
  simulator for both, and CocoaPods silently copies nothing from an
  xcframework slice that lacks one, so the app fails to link with
  "Framework 'libavcodec' not found". The macOS slice is universal too:
  Flutter builds release macOS apps for both architectures.
- Apple minimums are macOS 11 and iOS 16 (`tool/apple_framework.sh` and the
  podspec): the Xcode 27 SDK's libc++ warns below macOS 11 / iOS 15, and
  `AppleFrameConverter` uses `VTPixelTransferSession` (iOS 16).
- Real devices get media from `tool/serve_media.py`. Android: `adb reverse
  tcp:8765 tcp:8765`, then `http://127.0.0.1:8765/...`; Xiaomi phones
  refuse `adb install` (`INSTALL_FAILED_USER_RESTRICTED`) until someone
  confirms it on the phone. iOS: bind the server to the LAN (`0.0.0.0` as
  the third argument), `flutter build ios --release` (a debug build does
  not start without a debugger attached), `xcrun devicectl device install
  app` and `device process launch`; the developer certificate must be
  trusted on the device once, and the app asks for local network access.
- VideoToolbox hwaccel fails to initialize in the iOS simulator; the
  simulator decodes in software (`sw` in the example's stats).
- Run CocoaPods with `LANG=en_US.UTF-8`, or `pod install` aborts.
- `flutter test integration_test -d macos` builds into the same
  `build/macos/.../lychee_player_example.app` as `flutter build macos`, with
  the test as its entry point; launched by hand it shows a blank window.
  Rebuild the example before handing the binary to anyone.
- Asking VideoToolbox (through FFmpeg) for BGRA output fails at hwaccel init
  and FFmpeg quietly falls back to software decoding. Keep its default output
  and convert with `AppleFrameConverter`. Check `hw=1` in the CLI summary
  after touching decoder setup.
- Homebrew's FFmpeg is a GPL build and is linked only by the core's own
  CMake build (CLI, GLFW example, `tool/regression.sh`), for development.
- The Flutter plugin's Android side compiles no C++: it takes the core and
  the frame driver from the AAR `tech.soit.lychee:lychee`, which
  `sdk/android/gradlew -p sdk/android :lychee:publish` puts into
  `sdk/android/build/repo`. After changing `core/` or `sdk/android`, rerun
  it before building the Flutter example for Android, or the example runs
  the old core. The SDK's CMake forces `-O2` on the core: debug builds are
  `-O0`, and per-pixel frame work alone then caps 1080p at ~14 fps.
- A `SurfaceView` drops CPU buffers whose size after their transform is
  not the size given to `setFixedSize`, so rotated software frames need a
  window of the rotated size (`lychee_jni.cc`).
- On the development Mac the Kotlin compile daemon can hang `flutter build
  apk` for many minutes; prefix the command with
  `env 'ORG_GRADLE_PROJECT_kotlin.compiler.execution.strategy=in-process'`.
- Threads the core starts inherit the creator's priority, which on Android is
  the platform thread's (nice -10). Plugin threads that pull frames must run
  at display priority or software decoding starves them.
- On Linux (WSL Ubuntu 22.04), `TSAN=1` dies with "unexpected memory
  mapping" unless run under `setarch x86_64 -R`. With GCC 11 it then reports
  false double locks and races in `player.cc`: GCC 11's libtsan does not
  intercept `pthread_cond_clockwait`, which libstdc++'s `wait_for` uses.
  Use clang's TSan there (`CC=clang CXX=clang++`, in a fresh `build-tsan`:
  CMake reads them only on the first configure), with suppressions for the
  uninstrumented FFmpeg and libpulse. Software decoding under TSan is far
  slower than real time on Linux, so only the reports, not the timing
  checks, mean anything.
- The default output device on the development Mac is a virtual device that
  reports zero latency, so `latency 0 us` in the CoreAudio log is expected
  there. The CLI plays muted but still opens the default device; Bluetooth
  headphones take ~300 ms to start, so it measures drift from when the
  position first moves, not from READY.
