# Roadmap

State as of 2026-10-25: the core plays files and network streams through
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

1. **iOS audio sessions on a device**: the Flutter example (release, the
   prebuilt `lychee.xcframework`) on an iPhone 17 (iOS 27.0.1, 120 Hz)
   streams `h264_aac.mp4` from `tool/serve_media.py` over the LAN through
   VideoToolbox (`videotoolbox_vld`, `hw`) at 30.0 fps, 359 and 360 of
   360 frames in two runs, 0 dropped (2026-10-26); the 300-324 the
   simulator showed came from the busy Mac. Builds on a new device need
   `xcodebuild ... -allowProvisioningUpdates
   -allowProvisioningDeviceRegistration` once, and devicectl's `--console`
   shows the core's log (Dart `print` does not reach it in release). Not
   yet seen: audio session interruptions (a call) and route changes
   (unplugging headphones).
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

None known. Add them here with a done criterion.

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
   them in EDR. With `hdr_pq_bars.mp4` (100 to 4000 nits) the screen's
   EDR headroom rises to 7.3 on a MacBook Pro's XDR panel and 8.0 on an
   iPhone 17, and an HDR screen capture on the Mac reads the bars at 0.5,
   1.0, 1.7, 2.8 and 5.2 times SDR white (2026-10-26). On macOS/iOS 26
   and later `preferredDynamicRange` alone leaves EDR off (headroom 1.2)
   because VideoToolbox's surfaces carry no headroom, so the view sets
   `contentsHeadroom`. The panels' brightness was measured by headroom
   and capture, not by eye or photometer.
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
   - Android: in the SDK's `LycheePlayerView`, MediaCodec renders HDR into
     the SurfaceView itself: on a Xiaomi 14 (Android 16) the layer is
     `BT2020_ITU_PQ` with HDR10 metadata and the display's `hdrSdrRatio`
     goes from 1.0 to 5.7 (2026-10-26); software decoding is tone-mapped
     to SDR. The Flutter plugin's path (MediaCodec into a
     `SurfaceProducer`) is unchecked for HDR.
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
   someone with sudo there to `apt install mingw-w64 nasm`; then hosting,
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
   (Distribution 2). Unverified there: the Impeller renderer. Flutter
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
3. **Swift Package Manager**: add `flutter/darwin/lychee_player/Package.swift`;
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
   CA store as a file in the app's cache directory. Left before a release:
   the x86_64 half of the macOS slice was
   built without assembly (no `nasm` on the build Mac), so Intel Macs decode
   in software slower than they should; install `nasm` and rebuild
   `macos-x86_64`. AV1 decodes only through VideoToolbox (M3 and later):
   add dav1d (BSD) if software AV1 matters. Android before 11 finds the
   cache directory from the package name (no `TMPDIR`), never run there.
   Apps must ship the FFmpeg license notice and say where its source is;
   the AAR and the xcframeworks should carry a `THIRD_PARTY_NOTICES` and
   releases a link to the FFmpeg and mbedTLS sources. Only the
   development CMake build (CLI, GLFW example, `tool/regression.sh`
   without `FFMPEG_ROOT`) still links Homebrew's GPL FFmpeg.
2. **pub.dev packaging**: `flutter pub publish` only packs `flutter/`. The
   Apple pod now vendors prebuilt xcframeworks (`lychee`, FFmpeg) from
   `flutter/darwin/Frameworks`, and the Android plugin takes the AAR
   `tech.soit.lychee:lychee` from `sdk/android/build/repo`, but both are
   built locally and gitignored; Windows and Linux still compile `../core`
   from outside the package. So today the plugin works only as a path or git dependency.
   **Open question for the user**: where the prebuilt artifacts are hosted
   and how a fresh app gets them. Candidates: inside the package (pub.dev
   caps a package at 100 MB compressed; the Apple xcframeworks alone are
   tens of MB), release assets downloaded at build time (as the Windows and
   Linux plugins already download FFmpeg; for the pod, a `prepare_command`
   or script phase with pinned checksums), or a separate binary pod / Swift
   package. Done when `flutter pub publish --dry-run` produces a package
   that builds in a fresh app.
3. **Native SDKs, with Flutter as one consumer** (decided 2026-10-25).
   1. `sdk/android/` exists (2026-10-25): one AAR, `tech.soit.lychee:lychee`,
      with the core as `liblychee.so` (also a prefab module) and its
      FFmpeg, and the Kotlin `LycheePlayer`, `LycheeFrameDriver` and
      `LycheePlayerView` (a `SurfaceView`; MediaCodec rotates the video
      upright, `lychee_config.rotate_surface_frames`, API 6). The Flutter
      plugin's Android side is Kotlin only and uses the SDK's driver with
      the `SurfaceProducer`'s Surface. Left: hosting the AAR (item 2;
      today `sdk/android/gradlew :lychee:publish` into a local repo).
   2. `sdk/apple/` exists (2026-10-25: `LycheePlayer`,
      `LycheeFrameDriver`, `LycheePlayerView`; its example plays in
      hardware on macOS and in software on the iOS simulator, 0 dropped).
      Left: the Flutter plugin still has its own copy of the driver
      (`LycheeTexture` in `LycheePlayerPlugin.m`). It can use the SDK's
      only through SwiftPM (a pod cannot depend on a Swift package), and
      only by URL: Flutter adds plugin packages through symlinks in
      `.packages/`, and SwiftPM resolves a relative `path:` dependency
      against the symlink, not the plugin's real directory. A URL needs a
      `Package.swift` at the repository root and the xcframeworks as
      `binaryTarget(url:checksum:)` release assets, so this waits for
      item 2's hosting decision; do it together with Platforms 3.
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
