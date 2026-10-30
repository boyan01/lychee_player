# AGENTS.md

lychee_player is a media player built as a reusable C++ library with a C ABI
(`core/`), plus a Flutter plugin that is one thin consumer of it. README.md
has the layout and build commands.

Before starting a feature or a new platform, read `docs/roadmap.md`: it lists
what is unfinished, known defects, and the done criteria for each item, and
nothing that is done. Update it in the same change when you finish (delete the
item), start, or discover one.

## Autonomy

Decide and keep going; do not wait for the user's OK on API design,
implementation choices, test thresholds, docs, minimum OS versions, C ABI
changes (bump `LYCHEE_API_VERSION` with a history line) or new
dependencies that fit the design rules below. Put the reason in the commit
message and, where it matters later, in a code comment, the README or this
file; what was done and how it was checked stays in the commit. Commit each
finished item locally.

Finish a feature on every platform it applies to (macOS, iOS, Android,
Windows, Linux) in one piece of work, not one platform per round: this Mac
runs the iOS simulator and the Android emulator, and reaches the Windows
PC, WSL and the Ubuntu VM through the `flutter-remote-test` skill. Leave a
platform in `docs/roadmap.md` only for the part that needs the user's
hands, and say there exactly what that is. Likewise, take the items that
need no hands together, rather than one per session.

Stop and ask only for:

- Pushing, publishing or hosting anything: `git push`, pub.dev,
  crates.io, release assets, where prebuilt artifacts live. Running
  `.github/workflows/release.yml` is a release: it pushes a tag that
  publishes to pub.dev.
- Anything that needs the user's hands: a real device, a machine this Mac
  cannot reach (e.g. one with VAAPI), confirming on a phone, accounts,
  certificates. Mark the roadmap item as waiting on the user and move on
  to the next one.

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
  Backends are chosen in `media/factory.cc`. Android's base AAR is
  FFmpeg-free; its optional `lychee-ffmpeg` AAR registers the FFmpeg
  backend through `LycheeFFmpeg.load()` before any player is created.
  The private extension interface requires matching SDK sources and NDK;
  only the C ABI is public and stable across builds.
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
  frames for display times ahead of the clock. Input that cannot seek
  does not move the demuxer: the read thread keeps each video and audio
  stream's packets since the keyframe at or before the clock
  (`PacketHistory`, shared with the queues), and the resync pushes them
  into the flushed queues under the new generation before the read goes
  on. So such input reads every stream that keeps a history, selected or
  not; HLS reads other video streams (variants) only once selected (with
  FFmpeg and without; the FFmpeg-free `HlsDemuxer` then starts the
  variant a little before the read position and drops what the others
  already delivered on the audio they share).
- An audio device that pulls nothing for `AudioRenderer::kDeviceTimeout`
  (never started, or stalled) is stood in for by the control thread
  (`PullForStalledDevice`), which renders and drops audio just ahead of the clock, so
  playback goes on silently; the device takes over on its next pull.
- Looping does not seek: at the end of the input the read thread seeks
  the demuxer back to the start and pushes a boundary packet, and reads
  each later pass with timestamps a pass length later (`Player::loops_`;
  passes after the first span what the tracks cover, without codec
  priming or padding past them: `Player::SetLoopSpan`), so
  the clock, the audio device and the renderers run on without a stop. At
  a boundary a renderer drains and flushes its decoder and cuts or pads
  audio to the boundary, without a new generation. The position, frame
  pts and subtitles are mapped back to the file's timeline. Looping
  turned off after the read wrapped gives the renderers an end time at
  the pass playing (`ApplyLoopEndLocked`). Unseekable
  input or an unknown duration falls back to a seek from the control
  thread.
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

Test a change at the lowest layer that can see it, and run only the layers
it touches. A core change does not need a Flutter app: the CLI and the
platform's native harness see everything the core does, and Flutter in
between only adds a slow build and the texture bridge, which has tests of
its own. `tool/changes.sh` names the areas a branch touches (against
`main`, uncommitted files included); CI runs its jobs for the same areas.

Drive every app from its command line: the CLI, the native harnesses and
the Flutter example take the file or URL as an argument, `--seconds N`
to quit by themselves, and print a `summary:` line (decoder, `hw`,
rendered, dropped, position) to read instead of a screen. Never click,
type or move the pointer with `xdotool` or the like, and never read
numbers off a screenshot: if a check needs a control the app lacks
(seek, pause, a track), add a flag for it (`--key SEC:NAME`), as the
harnesses have. Screenshots are only for what a picture shows (upright,
sized, controls drawn), and pixels go through `tool/frame_check.py`.
Stop when the item's done criterion is met; something odd found on the
way goes into `docs/roadmap.md` rather than into a longer test run.

The layers, lowest first:

1. **Unit tests** (`core/test`, the tiny harness in `core/test/test.h`; no
   framework): pure algorithms and parsers, such as the clock, queues,
   frame selection, vsync estimation and demuxer boxes.
2. **Playback through the CLI**: `tool/regression.sh` builds the core,
   runs the unit tests, and plays a fixed media matrix through
   `examples/cli` (headless, simulated 60 Hz vsync, muted). It prints
   PASS/FAIL per case; `NO_HW=1` decodes in software, as on CI. Cases run
   in parallel (`JOBS`, default half the cores); `ONLY='seek-* rate-*'`
   runs just the matching ones. Most cases run on a virtual clock
   (`lychee_cli --virtual-time`): the CLI advances `lychee_now_us()` one
   vsync at a time, but only once the pipeline has decoded enough to play
   it (`lychee_testing_player_settled`), and audio goes to
   `NullAudioSink`, which renders when the clock moves. Media plays as
   fast as it decodes, and drift, drops and glitches do not depend on
   machine load; `stalls=N` counts vsyncs the pipeline never caught up
   with. The hooks live in `core/src/api/testing.h`, not in the public
   ABI. Cases that need real time (a slow or stalled server, and the
   `realtime-*` cases that check CoreAudio timing) set `realtime=1`; do
   not run them while a Flutter or Gradle build loads the machine. CI's
   hosted VMs are loaded, so CI sets `REALTIME_ADVISORY=1`: their drift,
   glitches and drops only warn there, so check timing changes locally
   on an idle machine.
   `frames` cases check the frame on screen by pixels (`--frame-ppm`
   and `tool/frame_check.py`): conversion, rotation, tone mapping.
   `audio` cases check what the audio device output for clicks
   (`--audio-dump` and `tool/audio_check.py`): seeks, pauses, rate
   changes and loops.
3. **The platform's native harness**: the smallest native app that takes
   the platform's frames to the screen (frame hand-off, conversion,
   rotation, HDR, the display link), with no Flutter in between, so it
   also tells core bugs from texture bridge bugs. Run it on the platform
   and check what the window drew by pixels rather than by eye:
   `--snapshot SEC:FILE` (twice, so the pts advance) piped into
   `python3 -I tool/frame_check.py MEDIA --log -`, which compares each
   with FFmpeg's frame at its pts, scaled into the video rectangle
   (README, "Checking frames by pixels"). The media `tool/regression.sh`
   generates has no color tags, so it also checks the library's
   convention for untagged video (BT.709 above 576 lines, BT.601 at or
   below), which every frame path has to follow. Where a platform has no `--snapshot` yet, look at a window screenshot:
   upright, sized right, frames advancing at the video's rate.
   - macOS: `build/examples/glfw/lychee_glfw FILE` (Metal,
     `CVPixelBuffer`), and the Swift package's example (Core
     Animation; its `--snapshot` goes through ScreenCaptureKit).
   - iOS: the Swift package's example on the "iPhone 16" simulator
     (`simctl launch --console-pty ... --snapshot SEC:FILE`).
   - Android: the Android SDK's example on an emulator (`Pixel_10`,
     windowed). For what needs no window, `lychee_cli` cross-builds
     with the NDK (`cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/
     android.toolchain.cmake -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=26
     -DANDROID_STL=c++_shared -DBUILD_SHARED_LIBS=ON
     -DLYCHEE_FFMPEG_ROOT=third_party/ffmpeg/android-x86_64`) and runs
     from `adb shell` in `/data/local/tmp` beside its libraries
     (`LD_LIBRARY_PATH=.`); `--android-surface` decodes through
     MediaCodec, in real time only.
   - Windows: `examples/glfw` (D3D11 textures), through the
     `flutter-remote-test` skill. `--seconds N` and `--key SEC:NAME`
     (README) run it unattended on every desktop platform.
   - Linux: `examples/glfw` (OpenGL; CPU frames, dmabufs with VAAPI), on
     WSL or the Ubuntu VM through the same skill. Those have no real
     GPU; the Windows machine's dual-boot Ubuntu 24.04 (`ssh
     boyan@192.168.31.212` when booted into it; X11, `DISPLAY=:1` once
     the user has logged in) does: an RTX 4070 Ti, and the i9-12900K's
     UHD 770, which drives the display from the mainboard's port since
     2026-10-27 and decodes through VAAPI (iHD). Test VAAPI with our
     FFmpeg (`tool/build_ffmpeg.sh linux-x86_64`, then
     `LYCHEE_FFMPEG_ROOT=third_party/ffmpeg/linux-x86_64` for the core's
     CMake and for `flutter build linux`, or the release's asset).
     NVIDIA's VAAPI driver hangs, and the player
     falls back to software after 4 s; `nvidia-vaapi-driver` is not
     installed there since 2026-10-27, so libva cannot pick it.
     `lychee_cli --dmabuf /dev/dri/renderDN` decodes into dmabufs on
     one GPU without a window.
4. **Bindings**: the Swift package (`swift test`), the Rust crates
   (`cargo test`), each against the media `tool/regression.sh` generates
   (`MEDIA_ONLY=1` generates it without building).
5. **The Flutter plugin**: only for changes under `flutter/`, on the
   platforms whose plugin code changed. It checks what only Flutter
   adds (texture registration, the frame hand-off into the engine, GL
   state under Impeller, Dart and plugin lifetime), not playback: a seek
   or a format that works in the native harness works here too. Count
   its drops only in a profile build on an idle machine; a debug build
   drops frames to the Dart JIT whatever the plugin does.
6. **Packaging**: only for changes to how releases are built.

What each area needs (run `tool/changes.sh`):

- **core**: build and run the unit tests plus the cases the change
  touches (`ONLY=...`, seconds) while iterating; the full
  `tool/regression.sh` once, when done, with zero build warnings
  (`-Wall -Wextra` and more, MSVC `/W4`, `core/CMakeLists.txt`; GCC,
  clang and MSVC differ, so a Linux or Windows run can find what macOS
  does not; the plugins' and the Android SDK's builds of the core make
  them errors). CI runs
  `NO_HW=1` plain, TSan and ASan on every core change, so leave those to
  it unless an area below asks for them. For changes to buffers, parsers
  or object lifetimes, run `ASAN=1 tool/regression.sh` (AddressSanitizer
  and UndefinedBehaviorSanitizer) yourself. `COVERAGE=1
  tool/regression.sh` shows which lines of `core/src` no test reaches
  (`build-coverage/coverage/index.html`); look at it when adding tests.
  For changes meant to be faster (decoding, conversion, seeking), compare
  `tool/bench.sh` before and after on an idle machine (fps, open and
  seek latency, peak memory per resolution; rows kept in
  `build-release/bench.tsv`, and `tool/bench_compare.py` compares the
  last commit's rows with the one before). For changes to object lifetimes, caches or
  pools, `tool/soak.sh` (minutes of looping, random seeks and file
  switches on the virtual clock) checks that memory and threads stay
  flat.
- **threads** (`pipeline/`, the C API, teardown): also `TSAN=1
  tool/regression.sh` (it fails on any ThreadSanitizer report).
- **parsers** (the FFmpeg-free demuxers): also `NO_FFMPEG=1
  tool/regression.sh` and `tool/fuzz.sh` (a minute of libFuzzer per
  parser under ASan and UBSan; Homebrew's LLVM; `FUZZ_TARGET=matroska`
  for one). Add each input it crashes on, minimized, to
  `core/fuzz/regressions/TARGET` with the fix.
- **apple**: `tool/build_apple.sh`, then `swift test` in `sdk/apple`
  (plays through the frame driver and checks the player's lifetime), and
  build its example (`sdk/apple/Example/Example.xcodeproj`, README has
  the commands) for macOS and the iOS simulator with zero warnings, run
  both with `--snapshot` into `tool/frame_check.py`, including
  `rotated.mp4` (`--size 405x720` on macOS) when the view's layout
  changed. For what ends up on screen (frame selection,
  conversion, rotation) also `lychee_glfw`. The plugin's Swift package
  build uses `LycheeFrameDriver`; its pod keeps a port of it
  (`LycheeTexture` in `LycheePlayerPlugin.m`): fix both until CocoaPods
  support is dropped (roadmap, Distribution 3). For the core without
  FFmpeg: `NO_FFMPEG=1 tool/build_apple.sh` and `swift test --traits
  NoFFmpeg`. When the shipped FFmpeg build changes:
  `FFMPEG_ROOT=third_party/ffmpeg/macos-arm64 tool/regression.sh`.
- **android**: `sdk/android/gradlew -p sdk/android :lychee:publish
  :ffmpeg:publish :example:assembleDebug` (zero warnings), install the example on an
  emulator or a phone and check snapshots of `h264_aac.mp4` and
  `rotated.mp4` with `tool/frame_check.py`, each with hardware decoding
  and without (the `url`, `hw` and `snapshot` intent extras; README,
  "Checking frames by pixels", and its `--matrix bt601` for hardware
  frames on the emulator); `tool/android_check.sh` does that for both
  files and also runs the core's unit tests on the device.
  `tool/android_extension_check.sh` additionally consumes both published
  AARs with R8, checks both frame paths and the loader's failure cases;
  CI's `android` job runs it. The Flutter plugin takes its frame driver from
  the SDK, so a change to `LycheeFrameDriver` also runs the Flutter
  example on Android.
- **windows**, **linux**: `tool/regression.sh` on that platform (Linux:
  `NO_HW=1`) and its `examples/glfw` harness, through the
  `flutter-remote-test` skill.
- **abi** (`lychee.h`): bump `LYCHEE_API_VERSION` with a history line,
  rerun `tool/rust_bindings.sh`, and run every binding that uses what
  changed (rust, apple, android, flutter). `python3 -I
  tool/abi_check.py` (CI's `abi` job, against the push's base) compares
  the header with the last `v*` release's and with where the branch left
  `main`, uncommitted edits included: it lists breaks (a declaration
  gone, a field moved, retyped or inserted before the end, a type or
  enum value changed; layouts on the host and, compiled but not run,
  on 32-bit and Windows targets) and additions, and fails when either has no bump
  over that header's version. New fields go at the end of a struct
  that starts with `struct_size`.
- **rust**: `cargo test`, `cargo clippy --all-targets` and `cargo doc
  --no-deps` in `rust/` with zero warnings (the test plays `h264_aac.mp4`
  and checks the player's lifetime), and `cargo run --example play --
  FILE frame.ppm`; look at the frame.
- **flutter**: `flutter analyze` in `flutter/` is clean, and you have run
  the example (`cd flutter/example && flutter build macos --profile`,
  then `build/macos/Build/Products/Profile/lychee_player_example.app/Contents/MacOS/lychee_player_example
  --seconds 4 /tmp/lychee-media/h264_aac.mp4`; on Linux
  `build/linux/x64/profile/bundle/lychee_player_example`), read its
  `summary:` line and exit code, and looked at one window screenshot
  when the change is about what it draws; say in your report what you
  did not see, e.g. audio you could not hear. On macOS and iOS the
  plugin builds two ways, with Swift Package Manager (on here) and with
  CocoaPods (`enable-swift-package-manager: false` under `flutter:
  config:` in the example's pubspec, not committed); a change to
  `flutter/darwin` needs both. For player lifetime or
  teardown (Dart or plugin): `cd flutter/example && flutter test
  integration_test -d macos`, which switches files repeatedly and checks
  that exactly one player's threads remain. On Android it runs the same
  way (`-d emulator-5554`), but `flutter test` reinstalls the app, so
  serve the media instead: `python3 tool/serve_media.py /tmp/lychee-media
  8765` and `--dart-define=LYCHEE_MEDIA=http://10.0.2.2:8765`. Plugin
  code for another platform: its example there (iOS: `flutter build ios
  --simulator`, `xcrun simctl install/launch`, `xcrun simctl io
  screenshot`; Android: `flutter build apk --debug`, `adb install`, `adb
  exec-out screencap`; Windows and Linux: `remote-test ... --project
  flutter/example` with `--seconds N FILE`), its `summary:` line
  rendering at the video's rate.
- **packaging** (`tool/release.sh`, `flutter/prebuilt.json`, the
  podspec's or the plugin Gradle's download, `Package.swift`, the release
  workflows): run the release steps into a scratch directory (README,
  "Releases"), ending with `tool/release.sh check`, which builds a fresh
  macOS, iOS-simulator and Android app against the packed package with
  the assets served locally (Apple with CocoaPods and with Swift Package
  Manager); then `git checkout` the pins it wrote.

Before committing C, C++ or Objective-C++, run `tool/format.sh`: it
formats the lines the branch changed (`.clang-format`, Chromium style;
older code is reformatted only when someone edits it), and CI's format
job fails on what it would change. Then run `tool/tidy.sh` (clang-tidy
with `.clang-tidy`, on the lines the branch changed) and fix what it
reports; `--all` checks all of `core/src` compiled on this host.
`lychee.h` must stay plain C99: `core/test/c_header_test.c` compiles it
as such with `-Wpedantic`.

Adding a test: put it in the lowest layer that can observe the
behaviour. A pure algorithm gets a unit test in `core/test`; anything the
CLI can observe gets a case in `tool/regression.sh`; a binding's
lifetime or API gets a test in its own package (`swift test`, `cargo
test`); only behaviour of the Flutter plugin itself (texture
registration, Dart lifetime, hot restart) goes into
`flutter/example/integration_test`. What only a screen shows stays a
harness snapshot checked by `tool/frame_check.py` (a screenshot where
there is none). When a new directory or file kind appears, give it an
area in `tool/changes.sh`.

CI (`.github/workflows/ci.yml`) checks the format of every push, runs
the ABI check for `lychee.h` changes, the core on macOS (plain, TSan, and ASan with UBSan, on hosted VMs), Linux and Windows (with `examples/glfw`'s build), the Apple builds with `swift test` (Flutter's analyze and
example build only for `flutter/` changes), the Android SDK with
`tool/android_check.sh` on an x86_64 emulator, and the Rust crates,
each only for its areas. Harness screenshots on the desktops, iOS and
phones stay local.

## Gotchas

- The Flutter plugin on iOS and macOS (`flutter/darwin`) and the Swift
  package (`sdk/apple`) do not compile the core: they link
  `flutter/darwin/Frameworks/lychee.xcframework`, built by
  `tool/build_apple.sh`. After changing anything under `core/`, rerun it
  before building either example on macOS or iOS, or the example runs
  the old core. They may use only `lychee/lychee.h`; the framework carries
  no other header (and a module map, so Swift can `import lychee`).
- Without local builds, the pod and the Android plugin take the core
  from the release `flutter/prebuilt.json` pins, also inside this
  repository: an example built from a fresh checkout runs the released
  core, not the checkout's. Local builds win only when complete:
  all six xcframeworks (lychee, FFmpeg's five) in
  `flutter/darwin/Frameworks` without a `.prebuilt` stamp (the pod's mark
  of a download), and `sdk/android/build/repo`. A release brings the
  xcframeworks its pins list.
- The plugin's iOS/macOS sources live in
  `flutter/darwin/lychee_player/Sources/lychee_player`, where Flutter's
  Swift Package Manager integration wants them; the podspec compiles the
  same files. Flutter links the plugin's package into the app through a
  symlink named after the pub path's directory (`.packages/flutter`), and
  SwiftPM resolves relative paths against the symlink and refuses
  targets and binaries outside the package, so `Package.swift` finds
  `sdk/apple` by an absolute path from `#filePath`. It uses the SDK's
  driver only with complete local frameworks; with the pinned ones it
  compiles the plugin's copy, since the released core may lack API the
  checkout's `sdk/apple` calls.
- Flutter's Swift Package Manager migration adds the plugin as a local
  package to plugin example projects. Its identity there is the
  directory's (`lychee_player`), not the symlink's (`flutter`), and Xcode
  then refuses to resolve the graph ("unable to override package");
  keep that `78DABEA22ED26510000E7860 /* lychee_player */` reference out
  of the example's `Runner.xcodeproj` (the migration tries again each
  build and backs out).
- SwiftPM takes `binaryTarget` URLs only over https: an http
  `LYCHEE_PREBUILT_URL` serves the pod but not the Swift package.
- The Rust bindings (`rust/lychee-sys/src/bindings.rs`) are generated and
  checked in. After changing `lychee.h`, rerun `tool/rust_bindings.sh`
  (`cargo install bindgen-cli`) and wrap the change in the `lychee` crate.
- Xcode 27 builds apps for macOS 12 and later only, so the Swift example
  targets macOS 12 while the package and the frameworks say 11.
- `CALayer.contents` set to a `CVPixelBuffer` shows nothing in the iOS
  simulator; `LycheePlayerView` sets the buffer's IOSurface and keeps the
  buffer until the next frame.
- `tool/build_ffmpeg.sh` enables only listed demuxers, decoders, parsers
  and protocols. A format that plays in the CLI (Homebrew's full FFmpeg)
  may fail in the apps: check new formats with `FFMPEG_ROOT=
  third_party/ffmpeg/macos-arm64 tool/regression.sh`, and add what is
  missing to the script's lists. Android's optional FFmpeg extension provides cleartext HTTP
  only (LGPL 2.1+); all TLS goes through `HttpURLConnection`, and AES-128
  HLS through `javax.crypto`. The cleartext fallback resolves up to five
  redirects before handing HTTPS back to DataSource. FFmpeg's HLS refuses
  a URL whose protocol FFmpeg lacks before it calls `io_open`, so without
  `tool/patches/ffmpeg-hls-https-io-open.patch` its fallback opens no
  https playlist or segment on Android ("Invalid data found"); keep the
  patch applying on a new FFmpeg. Its AES-128 segments (`crypto+https://`)
  are decrypted in `FFmpegDemuxer::IoOpen`, not by FFmpeg. `lychee_cli` from
  `adb shell` has no Java VM and cannot open HTTPS. Apps need Java to load
  `liblychee` by name (`System.loadLibrary("lychee")`) so `JNI_OnLoad`
  keeps the VM for both HTTP and AES.
- The Flutter plugin never links the system FFmpeg. Build the LGPL one once
  per platform with `tool/build_ffmpeg.sh PLATFORM <ffmpeg source>` into
  `third_party/ffmpeg/` (`macos-arm64` and `macos-x86_64`; `ios-sim-arm64`
  and `ios-sim-x86_64` for the simulator, `ios-arm64` for devices;
  `android-arm64-v8a`), then `tool/package_ffmpeg_apple.sh` and
  `tool/build_apple.sh` for Apple; the pod vendors the result from
  `flutter/darwin/Frameworks`. On Android the optional `lychee-ffmpeg` AAR
  links `third_party/ffmpeg/android-ABI` and carries it; the base AAR
  builds independently. Use the FFmpeg major version
  Homebrew has: the core follows the current FFmpeg API (e.g.
  `sws_free_context`). After a new FFmpeg build, rerun all of them and
  `pod install` in `flutter/example/macos` and `flutter/example/ios`.
  Subtitles stay basic (decided 2026-10-27): text, and bitmap subtitles
  as pictures; ASS shows as its plain text, with no libass.
- The iOS simulator slice must hold both arm64 and x86_64: Flutter builds the
  simulator for both, and CocoaPods silently copies nothing from an
  xcframework slice that lacks one, so the app fails to link with
  "Framework 'libavcodec' not found". The macOS slice is universal too:
  Flutter builds release macOS apps for both architectures.
- Apple minimums are macOS 11 and iOS 16 (`tool/apple_framework.sh` and the
  podspec): the Xcode 27 SDK's libc++ warns below macOS 11 / iOS 15, and
  `AppleFrameConverter` uses `VTPixelTransferSession` (iOS 16).
- An emulator on the Windows PC (its SDK has `lychee_api34`, `lychee_api30`
  and `lychee_api29`, WHPX) gets media from `tool/serve_media.py` run on
  the PC through `adb reverse tcp:8765 tcp:8765` (again after each boot).
  Its own network (10.0.2.2, or the LAN) silently drops single bytes from
  TCP streams (emulator 37.2, 2026-10-28: every 8.7 MB download lost 10
  to 16), which shows up as corrupt packets near the end of a file and
  opens that fail with "Invalid data found". Start the emulator, like any
  window there, from a scheduled task in the logged-in session.
- Real devices get media from `tool/serve_media.py`. Android: `adb reverse
  tcp:8765 tcp:8765`, then `http://127.0.0.1:8765/...`; Xiaomi phones
  refuse `adb install` (`INSTALL_FAILED_USER_RESTRICTED`) until someone
  confirms it on the phone. iOS: bind the server to the LAN (`0.0.0.0` as
  the third argument), `flutter build ios --release` (a debug build does
  not start without a debugger attached), `xcrun devicectl device install
  app` and `device process launch`; the developer certificate must be
  trusted on the device once, and the app asks for local network access.
  A device the profile does not list fails to install ("This provisioning
  profile cannot be installed"): build once with `xcodebuild ...
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration`.
  `device process launch --console` shows the core's log; Dart's `print`
  does not reach it in release, `stderr.writeln` does.
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
- On the Windows PC, `VSINSTALLDIR` is set without its trailing
  backslash, which breaks `vcvars64.bat` (no `cl` on PATH): clear it
  first (`set VSINSTALLDIR=`). `tool/regression.sh` runs there in Git
  Bash from that environment (the script's header).
- A window started over ssh on Windows is invisible: run GUI programs
  (`lychee_glfw`, the Flutter example) from a scheduled task in the
  logged-in session, keep them topmost without activating them, and take
  the `remote-test screenshot --pid` at least ~5 s before the run ends
  (the screenshot helper starts slowly).
- In WSL, `sudo` wants a password, but root needs none from the Windows
  side (`wsl.exe -d Ubuntu-22.04 -u root -- apt-get install ...`), so any
  package can be installed. It has `nasm`, `libva-dev`, `libgnutls28-dev`,
  `mingw-w64` and `libz-mingw-w64-dev` (2026-10-26), so `tool/build_ffmpeg.sh
  linux-x86_64` (GnuTLS) and `windows-x86_64` both build there, and `libglfw3-dev`,
  `libegl-dev`, `libgles-dev`, `xvfb`, `xdotool` and `imagemagick` for
  `examples/glfw`. Its mingw-w64 (8.0) headers lack AV1's DXVA
  structures, so its Windows FFmpeg has no AV1 (the script warns).
  Commands sent through Windows' ssh pass cmd.exe (8191 characters, its
  own quoting): copy scripts over with `scp` and run them from `/mnt/c`.
  Linux has no VAAPI here, so run its regression with `NO_HW=1`.
  `tool/regression.sh` on Ubuntu needs the FFmpeg CI uses to generate
  its media (BtbN's 9.0 GPL build first on `PATH`): apt's 6.1 writes
  files that fail `frame-hdr` and the `seek-mkv*` cases.
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
