# Roadmap

State as of 2026-10-26: the core plays files and network streams through
FFmpeg, with accurate seek, pull-based frame selection, playback rate
(WSOLA), looping and reopening, track selection, text and bitmap
subtitles, metadata and cover art. The Flutter plugin runs on macOS and iOS
(VideoToolbox), Android (MediaCodec), Windows (D3D11VA) and Linux (VAAPI,
never seen working).
Everything below is unfinished.

Each item says where the work goes and when it is done. Remove an item when it
is done; add one when you find a gap. Order within a section is priority.

## Unverified

Built, but never observed working. Cheap to close; do these before building
on top of them.

1. **iOS audio sessions on a device**: video plays on an iPhone; not yet
   seen are audio session interruptions (a call: the sink restarts its
   unit when it ends) and route changes (unplugging headphones: the sink
   rereads the output latency).
2. **Android hardware decoding beyond the happy path**: MediaCodec plays
   `h264_aac.mp4` and `hevc10.mp4` on a Xiaomi 14 (`c2.qti.*`, 0 dropped,
   seeks and 1.5x) and H.264 on the Pixel 10 emulator (`c2.goldfish.*`),
   where HEVC Main10 fails at the first frame and falls back to FFmpeg
   (2026-10-25). Going to the background and back resumes on the new
   Surface (emulator); on the Xiaomi a 3 s round trip ends with 360 of
   360 frames and 0 dropped, though there Choreographer kept pulling in
   the background, so only the CLI's `background` case exercises the
   expiry of frames nobody pulled. The headless emulator drops about a
   third of the frames even at 360p, so its counts say nothing about
   this. Not yet seen: AAudio route changes, a stream
   changing resolution midway, and a device
   with only one hardware decoder instance (the emulator's H.264 decoder
   once failed to start right after a file switch, and FFmpeg took over).
3. **Linux hardware decoding**: FFmpeg's VAAPI hwaccel decodes on the
   render node of Flutter's EGL display, and frames reach the plugin as
   dmabufs (`LYCHEE_FRAME_DMABUF`), imported a plane at a time as EGLImages
   and converted by a shader into the `FlTextureGL`. Neither Linux machine
   here has VAAPI (the VM's vmwgfx has no VA driver; WSL's d3d12 driver
   fails to initialize), so only the fallback has been seen: VAAPI is
   rejected and software frames are uploaded into the same GL texture
   (h264, hevc10 and vp9 with seeks: 0 dropped; 1-5 dropped at 1.5x on
   the VM; 2026-10-25). Done when an Intel or AMD machine shows `hw` with
   0 dropped for `h264_aac.mp4` and `hevc10.mp4` (P010), with colours
   matching software decoding. This is where media-kit had the most
   texture bugs; budget for it.

## Known defects

1. **Replacing the log callback from two callbacks at once deadlocks**:
   `lychee_set_log_callback` waits until no other thread is inside the
   callback, so two threads that both call it from inside the callback
   wait on each other. Done when the setter from inside a callback swaps
   the sink without waiting (the old one stays valid until every caller
   returns), with a unit test that runs two such threads.
2. **`realtime-loop` drifts on CI's hosted VM**: it plays on the real
   clock and audio device, so load shows up as drift, and CI's plain job
   failed on it once (2026-10-26). A red plain job needs its log read.
   Done when CI runs the `realtime=1` cases somewhere their timing means
   something, or not at all. The virtual-time flakes of that day
   (`live-resolution`, `live-jitter`, `subtitle-mid-cue`) are fixed: time
   ran ahead of a slow live decoder, and of events still waiting for the
   control thread's next 10 ms tick; 24 runs six at once now give the
   same counts every run.
3. **Events of players a hot restart left go to a closed isolate**: the
   plugins destroy the players behind their textures when the engine
   goes, and on `disposeAll`, which Dart calls before an isolate's first
   player (2026-10-26: macOS hot restart and Android back-out seen,
   integration test on both; Windows and Linux built only). Until that
   call, which never comes if the new isolate opens no player, the old
   players keep playing and send events to the old isolate's closed
   `NativeCallable`. No crash has been seen, but the integration test's
   orphan player registers no callback, so nothing covers it. Done when
   the plugin clears a player's event callback as soon as its isolate
   goes (or events cross through a bridge that drops messages for a dead
   port), with the integration test's orphan registering a callback.
   Development only: release builds have no hot restart.
4. **`seekable` is not in the C ABI**: since 2026-10-26 the core ignores
   a seek on input that cannot seek (an http server without range
   requests, through FFmpeg) and sends no `SEEK_COMPLETED`, but callers
   cannot tell beforehand, so a UI shows a seek bar that does nothing.
   If a seekable input rejects an attempted seek, playback now enters
   ERROR instead of continuing from an uncertain read position; HTTP
   regressions cover a server that starts rejecting ranges after open,
   for both user seeks and read-ahead loops. A track switch, new video
   device or decoder rebuild on input that cannot seek does not touch the
   demuxer and goes on from its read position, losing what was read ahead.
   Done when `lychee_player_is_seekable()` (an ABI bump) reaches the
   Dart, Swift, Kotlin and Rust wrappers and the Flutter example disables
   its slider.

Calling `lychee_player_open()` from an event callback returns -1 (API 9)
rather than reopening; callers reopen from their own thread. Unit tests
are off in a shared-library build (`LYCHEE_BUILD_TESTS` needs the static
core's internals).

## Features

1. **Live input in flutter-airplay**: the core plays pushed packets
   (`lychee_player_open_live`, ABI 5) with flutter-airplay's timeline,
   covered by the CLI's `live-*` cases (H.264, HEVC, ALAC, jitter,
   stalls, flushes, still screens, resolution changes; 2026-10-25). Left:
   AAC-ELD, which AirPlay mirroring sends, has never been decoded (FFmpeg
   cannot encode it without libfdk-aac, so there is no test file; capture
   one from a session); low-latency setup for MediaCodec
   (`KEY_LOW_LATENCY`) and other hardware decoders, which today get only
   FFmpeg's `low_delay`; no Dart API (flutter-airplay calls the C ABI from
   its own native code). Done when flutter-airplay plays a mirroring
   session through lychee behind a flag on macOS and Android, with A/V in
   sync on Bluetooth headphones.
   After a stall, audio moves up to 500 ms behind video and returns over
   a few seconds, as in flutter-airplay; video latency recovers within
   ~10 s (`live-stall`).
2. **HDR beyond Apple's SDK**: on Apple, `LYCHEE_FRAME_CVPIXELBUFFER_NATIVE`
   hands out VideoToolbox's buffers as they are (10-bit `x420` with PQ or
   HLG attachments; `native-hdr`), and the Swift `LycheePlayerView` shows
   them in EDR.
   8-bit frames from HDR are tone-mapped to SDR BT.709 (VideoToolbox's
   pixel transfer, swscale's perceptual intent for software decoding;
   `hdr-tonemap*`). HLG comes out darker through VideoToolbox (98 for a
   0.5 signal) than through swscale (142, what BT.2408 gives); that is
   Apple's mapping. Missing:
   - Flutter: textures are 8-bit SDR on every platform, so the plugin
     stays on tone-mapped BGRA.
   - Windows: the D3D11 video processor gets each frame's DXGI color
     space (`ID3D11VideoContext1`). On an RTX 4070 Ti it maps HLG to SDR
     (124 for a 0.5 signal) but clips PQ to white even with HDR10
     metadata, so PQ frames go through swscale like software decoding
     (181, as on Apple; `lychee_cli --d3d11` reads the texture back,
     2026-10-25). That costs a download per frame; a tone-mapping shader
     after the video processor would keep PQ on the GPU. HDR output needs
     an R10G10B10A2 or FP16 texture type.
   - Linux: `lychee_dmabuf` already carries P010 and the matrix, but not
     the primaries and transfer function a caller needs for HDR; needs a
     VAAPI machine to check.
   - Android: the SDK's SurfaceView shows MediaCodec's HDR output; the
     Flutter plugin's path (MediaCodec into a `SurfaceProducer`) is
     unchecked.
3. **libass in shipped builds**: styled ASS goes through libass
   (`LYCHEE_WITH_LIBASS`, pictures like bitmap subtitles) only where the
   core's own CMake build finds it with pkg-config: the CLI and GLFW
   example with Homebrew's libass. The plugin and the SDKs show ASS as
   plain text. Shipping it means building libass, FreeType, HarfBuzz
   (MIT-style) and FriBidi (LGPL 2.1, dynamic) per platform like
   `tool/build_ffmpeg.sh`, plus fonts: CoreText on Apple, fontconfig on
   Linux, DirectWrite on Windows, and a bundled default font on Android,
   which libass has no font provider for. Done when the Flutter example on
   macOS and Android shows `sub_ass.mkv` styled.

## Platforms

Every platform plays through FFmpeg software decoding and CPU RGBA frames
except Apple's (VideoToolbox, `CVPixelBuffer`), Android's (MediaCodec
rendering straight into the `SurfaceProducer`'s Surface), Windows'
(D3D11VA, shared D3D11 textures) and Linux's (VAAPI dmabufs, unverified
above). What is left per platform is a renderer in `examples/glfw`
(Metal only today) and its own `tool/regression.sh` run.

1. **Windows leftovers**: hardware decoding runs through FFmpeg's D3D11VA
   on Flutter's adapter, and frames reach Flutter's `GpuSurfaceTexture` as
   shared BGRA textures (h264, hevc10 and vp9 with seeks and 1.5x: 0
   dropped; the integration test passes; 2026-10-25). The FFmpeg DLLs
   (BtbN's LGPLv3 build, ~120 MB) should become a slim build of our own:
   a `windows-x86_64` case in `tool/build_ffmpeg.sh` cross-compiling with
   mingw-w64 in WSL (`--enable-d3d11va`, Schannel for TLS), which needs
   someone with sudo there to `apt install mingw-w64 nasm` (or a job in
   `release.yml`: hosted runners have sudo); the release then hosts it,
   as for Linux.
   Unverified there: the pixel buffer path for a Flutter without a D3D11
   adapter, multiple monitors, audible output, and `tool/regression.sh`
   (needs bash). The decoder is rebuilt at the position when the adapter
   changes (`lychee_player_set_video_device()`) or a hardware decoder fails
   midway, but only a simulated loss has run (`decoder-lost`); not yet seen:
   a real GPU reset (`dxcap -forcetdr`), which also loses the plugin's own
   device, and an adapter change during playback (the plugin sets the
   adapter once).
2. **Linux leftovers**: `tool/build_ffmpeg.sh linux-x86_64` builds the
   decode-only FFmpeg on Ubuntu 22.04 (WSL: 38 s, 8.2 MB of libraries
   against BtbN's 161 MB). It links the system's libva, libva-drm, libdrm
   and OpenSSL 3, so it is LGPL 3 like BtbN's (GnuTLS would keep LGPL
   2.1, but WSL has no sudo to install its headers; `nasm` and
   `libva-dev` came from `apt-get download`). With it, `NO_HW=1
   FFMPEG_ROOT=... tool/regression.sh` passes in WSL except
   `realtime-loop`, which fails the same way with BtbN's (WSLg's audio
   clock, below), and the Flutter example built in WSL with
   `LYCHEE_FFMPEG_ROOT` plays `h264_aac.mp4` on the 26.04 VM (360 of 360
   frames, 0 dropped; 2026-10-25), so a 22.04 build runs on 26.04. The
   plugin still downloads BtbN's: shipping ours needs a place to host it
   (Distribution 2: a job in `release.yml` and a pin in
   `flutter/prebuilt.json`, which the Linux CMake would read in place of
   BtbN's URL; arm64 Linux still needs one too). Unverified there: the Impeller renderer. Flutter
   makes Skia forget its GL state around a texture's populate but does
   nothing for Impeller (`ResolveTextureImpeller`), so populate now saves
   and restores what it changes. Whether that is enough is unknown: on the
   VM, Impeller (OpenGLESSDF on vmwgfx) garbles a plain Flutter app with
   no plugin as well (a Slider half drawn, text missing, colours not
   updating; 2026-10-25), so only the integration test (lifetime) passes
   there. Done when a machine with a real GPU shows the example playing
   under Impeller with its controls drawn.
   WSL's audio device (WSLg's `RDPSink`) runs 0.3-0.4% slower than the
   system clock in most runs: on the real clock, video follows it by
   holding a frame for 3 vsyncs every ~3 s (`glitches=1-4`,
   `max_drift` 30-45 ms over 12 s), with or without a seek
   (2026-10-25). That is lip sync working, not a seek problem: on virtual
   time, with no audio device, plain playback shows none. Not yet seen
   on a machine with a sound card.
3. **32-bit Android**: the AAR has the core for `arm64-v8a` and `x86_64`
   only (decided 2026-10-26); apps restrict `abiFilters` to those
   (Flutter README), and on a 32-bit device the plugin no longer crashes
   the app at start: `LycheePlayer.create()` fails in Dart. Some TV boxes
   run a 32-bit system on 64-bit chips; supporting them needs
   `tool/build_ffmpeg.sh android-armeabi-v7a`, the ABI in the AAR and the
   release, and a 32-bit device or emulator to run on.
4. **Swift Package Manager**: add `flutter/darwin/lychee_player/Package.swift`;
   Flutter already warns that CocoaPods-only plugins will become an error.
   The xcframeworks in `flutter/darwin/Frameworks` can be its
   `binaryTarget`s. It should depend on the `sdk/apple` package for the
   frame driver (Distribution 3.2 says why that takes a URL).

## FFmpeg-free builds

Apple without FFmpeg plays MP4 (2026-10-25): `-DLYCHEE_WITH_FFMPEG=OFF`
builds `Mp4Demuxer` (`backends/mp4`), `VTVideoDecoder` (H.264, HEVC; NV12
or 10-bit NV12, as FFmpeg's hwaccel outputs, with VideoToolbox's software
decoder as the fallback on macOS) and `ATAudioDecoder` (AAC including
ADTS, ALAC) over a `DataSource` (files; http(s) through NSURLSession, with
range requests and reconnects). The iOS core is then 425 KB instead of
~7 MB of FFmpeg per architecture. `NO_FFMPEG=1 tool/regression.sh` runs
every case whose media is MP4 (http faults, live input, seeks, loops,
decoder loss, HDR included) and passes. Decisions:

- Reordering needs no SPS parser: a decoded frame goes out once a packet
  with `dts >= its pts` was decoded (no later frame can come before it),
  the rest at the end of the stream; live input (`low_delay`) outputs at
  once.
- The MP4 parser is our own (~700 lines) rather than Chromium's
  `media/formats/mp4`, which is built around MSE's fragmented MP4.
- The FFmpeg build keeps FFmpeg's demuxer and hwaccel; the new backends
  are compiled only without FFmpeg, so the shipped xcframework does not
  carry dead code.

Left:

1. FFmpeg reading through `DataSource` via a custom `AVIOContext`, so both
   builds share IO behaviour.
2. Containers: fragmented MP4, MPEG-TS, HLS, WebM; tx3g (mov_text)
   subtitles, which the MP4 demuxer skips. MP3 and Opus/FLAC in MP4 are
   listed but have no decoder.
3. Packaging: a `tool/build_apple.sh` variant (or the Swift package) that
   ships the FFmpeg-free core; iOS compiles (arm64, zero warnings) but has
   not run, and the plugin and Swift package still link FFmpeg.
4. Windows: decode with Media Foundation (flutter-airplay's
   `windows_video.cpp`: H.264/HEVC MFTs on D3D11) into the same
   `LYCHEE_FRAME_D3D11_TEXTURE` frames, ahead of FFmpeg's D3D11VA, which
   stays the fallback when FFmpeg is present (HEVC without the Store's
   HEVC extension, VP9). Audio through Media Foundation's AAC decoder.
5. Android: MediaCodec already decodes video without FFmpeg; audio needs
   MediaCodec too.
6. Malformed input: the MP4 parser bounds sample tables, single samples
   (64 MiB, inside the file) and its allocations (`huge-sample-table`,
   `huge-sample`, `truncated-file`, `corrupt-file`), but has never been
   fuzzed. A libFuzzer target over `Mp4Demuxer` on an in-memory
   `DataSource`, run under ASan, before it ships.

The goal is FFmpeg-free builds on Apple platforms, Windows and Android.
Linux keeps FFmpeg: it has no system audio decoder or demuxer.

## Distribution and infrastructure

1. **LGPL FFmpeg**: every plugin build now ships an LGPL-only FFmpeg as
   dynamic libraries: macOS and iOS as xcframeworks from
   `tool/build_ffmpeg.sh` + `tool/package_ffmpeg_apple.sh` (the macOS
   example loads only them, 2026-10-25), Android from `tool/build_ffmpeg.sh`,
   Windows and Linux as BtbN's LGPLv3 downloads (slimming them: Platforms
   1 and 2). The Apple and Android builds enable only the formats the core
   plays (about 7 MB per architecture, from 15; 2026-10-25), and
   `FFMPEG_ROOT=... tool/regression.sh` checks them. Android's links
   mbedTLS for https, so it is LGPLv3; the core gives mbedTLS Android's
   CA store as a file in the app's cache directory. Releases build
   `macos-x86_64` with assembly (`tool/release.sh build` requires `nasm`;
   the build Mac's local one has none). AV1 decodes only through VideoToolbox (M3 and later):
   add dav1d (BSD) if software AV1 matters. Android before 11 finds the
   cache directory from the package name (no `TMPDIR`), never run there.
   Apps must ship the FFmpeg license notice and say where its source is;
   releases attach the FFmpeg and Mbed TLS sources they were built from,
   and `THIRD_PARTY_NOTICES` (Chromium, FFmpeg, Mbed TLS, with the license
   texts) goes into `lychee.framework`, the AAR (as the Java resource
   `META-INF/lychee/THIRD_PARTY_NOTICES`, so it lands in APKs) and the
   pub package (`core/`). Only the
   development CMake build (CLI, GLFW example, `tool/regression.sh`
   without `FFMPEG_ROOT`) still links Homebrew's GPL FFmpeg.
   BtbN's Windows and Linux builds link OpenSSL, dav1d, libvpx, libaom,
   Opus, LAME, libass and more statically (their avcodec configuration),
   whose notices we do not ship; `THIRD_PARTY_NOTICES` now says so and
   names BtbN's revision (n9.0.2-17-g2a571b6068), which is not the
   ffmpeg-9.0.2 source the releases attach. Our own FFmpeg-only builds
   (Platforms 1 and 2) fix both.
   The playback matrix runs through the shipped macOS FFmpeg
   (`FFMPEG_ROOT=third_party/ffmpeg/macos-arm64`) in CI's flutter-macos
   job and in `release.yml` before anything is packed (2026-10-26; its
   first GitHub run failed only on the live flakes fixed since). Not
   covered there: the Swift, Rust, Android and Flutter lifetime tests,
   which run only by hand.
   `tool/release.sh build` reuses any `third_party/ffmpeg/PLATFORM` that
   exists, so a changed `tool/build_ffmpeg.sh` needs that directory
   deleted by hand (CI keys its cache on the script, so it rebuilds);
   a stamp with the script's hash would make local builds safe too.
2. **First release**: prebuilt artifacts are assets of the version's
   GitHub release (`boyan01/lychee_player`, tag `vVERSION`), pinned by
   SHA-256 in the package (decided by the user 2026-10-26: one host for
   pub, SwiftPM and Gradle, no accounts, no binaries in git or in the
   package, and SwiftPM's `binaryTarget` takes only URLs anyway).
   `flutter/prebuilt.json` pins them for the pod (unzips the xcframeworks
   into `darwin/Frameworks`) and the Android plugin (lays the AAR and POM
   out as a Maven repository in `~/.cache/lychee_player`); the root
   `Package.swift` pins the same zips for SwiftPM. Local builds win.
   `LYCHEE_PREBUILT_URL` names a mirror, for users where GitHub is slow;
   SwiftPM ignores it. Windows and Linux compile the core from a copy
   that `tool/release.sh` puts into `flutter/core` while pub packs (a
   symlink would do, but `remote-test` snapshots refuse symlinks).
   `release.yml`, dispatched with a version, builds, pins, commits, runs
   `tool/release.sh check`, drafts the release and pushes the commit and
   tag with a deploy key (a push with the workflow's token starts no
   workflow); `publish.yml`, started by the tag, publishes the release and
   the package through pub.dev's OIDC, which accepts only tag pushes.
   pub.dev already has `lychee_player` 0.4.0 (the old audio player,
   publisher `soit.tech`), so 1.0.0 is its next version and no first
   manual upload is needed.
   Android apps keep the AAR's arm64-v8a/x86_64 ABI filters only for a
   universal APK. Split APK builds use Flutter's filters instead, with
   `--target-platform=android-arm64,android-x64`; `check` builds and
   inspects both modes.
   Checked here (2026-10-26): every step of `release.yml` run by hand for
   1.0.0; `check` builds a fresh app for macOS, the iOS simulator and
   Android from the packed package (416 KB) with the assets served
   locally, downloading every pinned file, and SwiftPM's checksums match
   the pins; that macOS app plays `h264_aac.mp4` to its last frame in
   hardware on the downloaded frameworks; a fresh Windows and Linux (WSL) app builds from the packed
   package (`remote-test`, Flutter 3.47.7), as does the repository's
   example; the repository's macOS and Android example still build on
   the local artifacts, with nothing downloaded.
   1.0.0-beta.1 is out (2026-10-26): `release.yml` and `publish.yml` ran
   on GitHub (deploy key push, OIDC publish), the GitHub release is a
   prerelease with 10 assets, and pub.dev lists the beta while `latest`
   stays 0.4.0. A fresh app depending on `lychee_player: 1.0.0-beta.1`
   from pub.dev builds on all five platforms (macOS and iOS simulator
   here, Android, Windows and Linux via WSL), and on macOS (sandboxed,
   with `network.client`) and the Android emulator it plays
   `h264_aac.mp4` over http in hardware, with the frameworks and the AAR
   downloaded from the release. Left: 1.0.0, once the beta has been used.
   Later: the AAR on Maven Central too (what native Android users
   expect; needs a Sonatype account for `tech.soit`); the Windows and
   Linux FFmpeg as release assets in place of BtbN's (Platforms 1 and 2);
   strip the AAR's prefab `liblychee.so` (6 MB per ABI against 0.3 MB in
   `jni/`).
3. **Native SDKs, with Flutter as one consumer** (decided 2026-10-25).
   1. `sdk/android/` exists (2026-10-25): one AAR, `tech.soit.lychee:lychee`,
      with the core as `liblychee.so` (also a prefab module) and its
      FFmpeg, and the Kotlin `LycheePlayer`, `LycheeFrameDriver` and
      `LycheePlayerView` (a `SurfaceView`; MediaCodec rotates the video
      upright, `lychee_config.rotate_surface_frames`, API 6). The Flutter
      plugin's Android side is Kotlin only and uses the SDK's driver with
      the `SurfaceProducer`'s Surface. The AAR and its POM are release
      assets (item 2), which the Flutter plugin lays out as a Maven
      repository; native apps need Maven Central (item 2, later).
   2. `sdk/apple/` exists (2026-10-25: `LycheePlayer`,
      `LycheeFrameDriver`, `LycheePlayerView`; its example plays in
      hardware on macOS and in software on the iOS simulator, 0 dropped).
      Left: the Flutter plugin still has its own copy of the driver
      (`LycheeTexture` in `LycheePlayerPlugin.m`). It can use the SDK's
      only through SwiftPM (a pod cannot depend on a Swift package), and
      only by URL: Flutter adds plugin packages through symlinks in
      `.packages/`, and SwiftPM resolves a relative `path:` dependency
      against the symlink, not the plugin's real directory. The URL
      exists now: the root `Package.swift` is `sdk/apple` with the
      release's xcframeworks (item 2). Do it together with Platforms 3,
      after the first release.
   3. `rust/` exists (2026-10-25): `lychee-sys` (checked-in bindgen
      output, CMake build of the core as a shared library) and the safe
      `lychee` crate (`Player` is `Send + Sync`, `Frame` releases on drop);
      its test and example play `h264_aac.mp4` on macOS, with Homebrew's
      FFmpeg and with the LGPL one. Left: not published to crates.io;
      never built for Windows, Linux, Android or iOS (`LYCHEE_LIB_DIR` is
      meant for the AAR's and the xcframework's library, untried). The
      Apple FFmpeg from `tool/build_ffmpeg.sh` has `@rpath` install names
      (2026-10-25), so a Rust binary ships `liblychee` and FFmpeg next to
      itself with an `@executable_path` rpath (README); tried by moving
      the example with its libraries to another directory.
   Windows and Linux native users take the C ABI as it is.
4. **CI on Linux**: CI runs only on macOS (the regression with and
   without TSan, the Flutter macOS build). A `NO_HW=1 tool/regression.sh`
   job on `ubuntu-24.04` (FFmpeg from apt or `tool/build_ffmpeg.sh
   linux-x86_64`, the audio cases on a null sink) would catch Linux build
   breaks that only a WSL run finds today. TSan stays on macOS: on Linux
   it needs clang, suppressions for FFmpeg, and is far slower than real
   time. Done when the job runs on every push and is green.
