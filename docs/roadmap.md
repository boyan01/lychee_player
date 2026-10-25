# Roadmap

State as of 2026-10-25: the core plays files and network streams through
FFmpeg, with accurate seek, pull-based frame selection, playback rate
(WSOLA), looping and reopening, track selection, text subtitles and
metadata. The Flutter plugin runs on macOS and iOS (VideoToolbox), Android
(MediaCodec), Windows (D3D11VA) and Linux (VAAPI, never seen working).
Everything below is unfinished.

Each item says where the work goes and when it is done. Remove an item when it
is done; add one when you find a gap. Order within a section is priority.

## Unverified

Built, but never observed working. Cheap to close; do these before building
on top of them.

1. **iOS on a device**: the release example builds with the device FFmpeg
   (`tool/build_ffmpeg.sh ios-arm64`, then `tool/package_ffmpeg_apple.sh`),
   installs on an iPad Pro 11" (M1), launches and streams from
   `tool/serve_media.py` over the LAN (2026-10-25), back when the pod
   compiled the core from source; not yet run with the prebuilt
   `lychee.xcframework` (`tool/build_apple.sh`). Not yet seen on its
   screen: `hw` in the stats line (VideoToolbox), the frame count, audio
   session interruptions (a call) and route changes (unplugging
   headphones). The simulator decodes in software because VideoToolbox
   hwaccel fails there.
   On a busy Mac (load 4-7) the simulator showed 300-324 of 360 frames
   with `dropped 0`: the frame selection skips frames by cadence when the
   pull rate falls below the frame rate, and those do not count as
   dropped. Check the count on a device; if it is short there too, find
   out why the display link pulls slower than 60 Hz.
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

1. **Linux RSS grows on file switches**: about 40 MB per switch with
   glibc's default malloc, without a plateau over 30 switches; flat at
   ~740 MB with `MALLOC_ARENA_MAX=2`. Each player's new threads get their
   own malloc arenas, which keep the freed 1080p frames. Fix: pool frame
   buffers across players, or `malloc_trim` after a player is destroyed.
2. **Clock drift after a loop**: `lychee_cli --loop` reports `max_drift`
   around -60 ms after the jump back to the start, against ~-10 ms before
   (macOS and Windows). The audio clock is probably re-anchored before the
   first post-loop audio reaches the device. Done when drift after a loop
   matches drift before it, with a regression case asserting it.
3. **Glitches after a seek on Linux**: `seek-forward` shows exactly 3
   glitches in every WSL run (the case allows 3), 0 on macOS. Probably the
   PulseAudio stream's latency estimate settling after the flush. Done when
   it shows 0-1 there.

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
2. **HDR / 10-bit**: frames are converted to 8-bit BGRA, so HDR is clipped.
   Needs a 10-bit or YUV handle type plus color metadata across the C API,
   and EDR on macOS.
3. **Subtitles beyond plain text**: text cues (SRT, WebVTT, mov_text, ASS
   text with tags stripped) work. Missing: ASS styling and positioning
   (libass), bitmap subtitles (PGS, DVB, VobSub), and cues that started
   before a subtitle track is switched on mid-file (they appear only from
   the next cue).
4. **Artwork**: `lychee_player_get_metadata` returns text tags only;
   attached pictures are not exposed.

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
   (BtbN's LGPLv3 build, ~120 MB) should become a slim build of our own.
   Unverified there: the pixel buffer path for a Flutter without a D3D11
   adapter, multiple monitors, audible output, and `tool/regression.sh`
   (needs bash). The decoder is rebuilt at the position when the adapter
   changes (`lychee_player_set_video_device()`) or a hardware decoder fails
   midway, but only a simulated loss has run (`decoder-lost`); not yet seen:
   a real GPU reset (`dxcap -forcetdr`), which also loses the plugin's own
   device, and an adapter change during playback (the plugin sets the
   adapter once).
2. **Linux leftovers**: the bundled FFmpeg (BtbN, LGPLv3, ~130 MB) should
   become a decode-only `tool/build_ffmpeg.sh linux-x86_64` build (smaller,
   LGPL 2.1). Unverified there: the Impeller renderer (the VM runs with it
   off; the texture's GL state handling relies on Skia resetting its own),
   and a build from 22.04 running on 26.04.
3. **Swift Package Manager**: add `flutter/darwin/lychee_player/Package.swift`;
   Flutter already warns that CocoaPods-only plugins will become an error.
   The xcframeworks in `flutter/darwin/Frameworks` can be its
   `binaryTarget`s. It should depend on the `sdk/apple` package for the
   frame driver (Distribution 4.2 says why that takes a URL).

## FFmpeg-free builds

The interfaces allow it (`LYCHEE_WITH_FFMPEG=OFF` compiles but cannot open
anything). Pieces, in order:

1. `DataSource` (file + HTTP on platform networking), and FFmpeg reading
   through it via a custom `AVIOContext`, so both builds share IO behaviour.
2. Direct VideoToolbox / AudioToolbox decoders. The hard part is output
   reordering: VideoToolbox emits decode order, so the reorder depth must
   come from an H.264/HEVC SPS parser (port Chromium's
   `media/parsers/h264_parser.cc`, `h265_parser.cc`).
3. Container parsers ported from Chromium (`media/formats/mp4`, `mp2t`,
   `webm`) and an HLS playlist parser.
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
   1 and 2). Left before a release: the x86_64 half of the macOS slice was
   built without assembly (no `nasm` on the build Mac), so Intel Macs decode
   in software slower than they should; install `nasm` and rebuild
   `macos-x86_64`. Only the development CMake build (CLI, GLFW example,
   `tool/regression.sh`) still links Homebrew's GPL FFmpeg.
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
3. **Example app**: a file picker instead of a path field.
4. **Native SDKs, with Flutter as one consumer** (decided 2026-10-25).
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
   3. `rust/`: `lychee-sys` (bindgen over `lychee.h`, CMake build) and a
      safe `lychee` crate; callbacks are `Send`, frames release on drop.
   Windows and Linux native users take the C ABI as it is.
