# Roadmap

What is left to do, and nothing else. Each item says what is missing and
when it is done. Delete an item when it is done, and a sentence when what
it says is no longer true; add one when you find a gap. What was done and
how it was checked goes into the commit message; how something works goes
into a code comment, the README or AGENTS.md. Order within a section is
priority.

## Unverified

Built, but never observed working. Cheap to close; do these before
building on top of them.

1. **Android hardware decoding beyond the happy path**: not yet seen are
   AAudio route changes to a USB-C headset, and switching AirPods back to
   the speaker after clock correction (waiting on the user and the Xiaomi
   14: the prior run jumped ~0.2 s and dropped 8 frames; verify the step is
   smaller than a frame). Also missing are a stream
   changing resolution midway, and a
   device with only one hardware decoder instance (the emulator's H.264
   decoder once failed to start right after a file switch, and FFmpeg
   took over), and Android before 11, where the core finds its cache
   directory from the package name (no `TMPDIR`). Only the CLI's
   `background` case exercises the expiry of
   frames nobody pulled: Choreographer kept pulling in the background on
   the Xiaomi 14.
2. **Windows**: a real GPU reset (`dxcap -forcetdr`, which also loses the
   plugin's own device; only the simulated `decoder-lost` has run), an
   adapter change during playback (the plugin sets it once), HEVC on a
   machine without the Store's HEVC Video Extensions (should fall back to
   D3D11VA at open), the pixel buffer path for a Flutter without a D3D11
   adapter, multiple monitors, audible output, swscale's BT.709 for
   untagged HD (seen on macOS only), AV1 in the release's FFmpeg (WSL's
   mingw-w64 8.0 headers have no AV1 DXVA structures; the runner's
   may), `lychee_cli` with that FFmpeg, and `tool/regression.sh` (needs
   bash).
3. **Linux**: VAAPI on AMD, Wayland, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still).
4. **Apple without FFmpeg**: AV1 on an M3 or A17 Pro, and VP9 on a
   device with iOS 26.2 or later (waiting on the user): the iOS 26.5
   simulator has no VP9 decoder (`VTDecompressionSessionCreate` -12906,
   2026-10-28).

## Known defects

- **Timestamps restarting inside a single TS file with FFmpeg**:
  `FFmpegDemuxer` passes FFmpeg's timestamps on as they are, so
  `ts_timestamp_reset.ts` (two TS files joined without remuxing) reports
  3.021 s instead of about 6 s, drifts by up to 3 s, and seeks into the
  second half land in the first. The FFmpeg-free `TsDemuxer` joins such
  runs (`TsTimeline`); do the same for FFmpeg's `mpegts` (formats with
  `AVFMT_TS_DISCONT`). Done when `tool/regression.sh`'s
  `ts-timestamp-reset` and `seek-ts-reset*` cases, now run only with
  `NO_FFMPEG=1`, pass in both builds.
- **HLS audio renditions with FFmpeg**: FFmpeg's HLS demuxer does not
  put an audio rendition's timestamps on the variant's timeline when
  they start elsewhere: `hls_alt/master.m3u8` (TS video from 1.4 s,
  audio renditions in TS and in fMP4 from 0) plays 1 s out of sync
  (`max_drift=1016ms`), with the tone at 0.02 s instead of 0.95 s, and
  its tracks have no titles. The FFmpeg-free `HlsDemuxer` shares each
  discontinuity's offset between renditions. Done when
  `http-hls-renditions*`, now run only with `NO_FFMPEG=1`, pass in both
  builds.

1. **Slow HLS seeks past a discontinuity with FFmpeg**: FFmpeg's HLS seek
   drops every packet past a discontinuity where timestamps start over,
   so `FFmpegDemuxer` reads from before the first discontinuity (from the
   start if none has been read yet). Long streams make that slow; only an
   HLS demuxer that knows the segments, as the FFmpeg-free one does,
   would do better.
2. **iOS foreground recovery needs device verification** (waiting on the
   user): an iPhone 17 (2026-10-27) rejected 32 video packets (`-12909`)
   after an apparent interruption or background transition; the actual
   lifecycle trigger is unconfirmed. The native and Flutter frame drivers
   now request a fresh hardware decoder after becoming active following
   a background transition. Verify that device scenario resumes promptly,
   including a paused player. Also distinguish an audio interruption from
   an actual background transition: an alarm alone may not enter this hook.
   The simulator cannot reproduce this hardware
   failure; direct C ABI consumers must call
   `lychee_player_rebuild_video_decoder()` on their platform's recovery
   event. Released plugin binaries pinned to API 11 do not have this hook.

## Features

1. **Live input in flutter-airplay**: the core plays pushed packets
   (`lychee_player_open_live`). Left: AAC-ELD, which AirPlay mirroring
   sends, has never been decoded (FFmpeg cannot encode it without
   libfdk-aac; capture a file from a session); low-latency setup for
   MediaCodec (`KEY_LOW_LATENCY`) and other hardware decoders, which get
   only FFmpeg's `low_delay`. After a stall, audio moves up to 500 ms
   behind video and returns over a few seconds, as in flutter-airplay.
   Done when flutter-airplay plays a mirroring session through lychee
   behind a flag on macOS and Android, with A/V in sync on Bluetooth
   headphones.
2. **HDR beyond Apple's SDK**:
   - Flutter: textures are 8-bit SDR on every platform, so the plugin
     stays on tone-mapped BGRA.
   - Windows: PQ is tone-mapped by our shader like swscale (saturated
     colors at or above white approximate it: swscale clips in IPT, the
     shader in ICtCp). Left: HLG through the shader too (the D3D11 video
     processor gives 124 for a 0.5 signal, swscale 142), and HDR output,
     which needs an R10G10B10A2 or FP16 texture type.
   - Linux: `lychee_dmabuf` carries P010 and the matrix, but not the
     primaries and transfer function a caller needs for HDR; the
     dual-boot PC's Intel iGPU can check it.
   - Android: the Flutter plugin's path (MediaCodec into a
     `SurfaceProducer`) is unchecked, waiting on the user's HDR phone; the
     emulator has no HEVC Main 10 decoder. If the plugin shows PQ washed
     out, ask the decoder for SDR (`color-transfer-request`, API 31) for
     the plugin's surface only, behind a `lychee_config` flag (the SDK's
     SurfaceView must keep HDR).
   - Apple: VideoToolbox's tone mapping of bright HDR colors differs from
     swscale's reference (`hdr_pq_bars.mp4`: 27.9 dB), and HLG comes out
     darker (98 for a 0.5 signal against 142); that is Apple's mapping.
3. **32-bit Android**: the AAR has `arm64-v8a` and `x86_64` only
   (decided 2026-10-26). Some TV boxes run a 32-bit system on 64-bit
   chips; supporting them needs `tool/build_ffmpeg.sh
   android-armeabi-v7a`, the ABI in the AAR and the release, and a 32-bit
   device or emulator to run on.
4. **Software AV1**: the shipped FFmpeg has no AV1 software decoder, so
   AV1 plays only where the GPU decodes it (on Apple, M3 and A17 Pro and
   later). Add dav1d (BSD) to `tool/build_ffmpeg.sh` if it matters.

## FFmpeg-free builds

The goal is FFmpeg-free builds on Apple platforms, Windows and Android;
Linux keeps FFmpeg (no system audio decoder or demuxer). Apple plays MP4,
Matroska/WebM, MPEG-TS and HLS without it, and ships
`lychee_noffmpeg.xcframework`; Android and Windows play MP4 files.

1. **Shared IO on Android and Windows**: an http `DataSource` there
   (`HttpURLConnection` or Cronet through JNI, WinHTTP), which their
   FFmpeg-free builds need anyway; then FFmpeg reads through it as on
   Apple. One `NSURLSession` per demuxer would let HLS segments reuse
   connections.
2. **Containers**: HLS switches variants only when a track is selected,
   not by measured bandwidth; subtitle renditions (WebVTT) are not
   read. AC-3 has
   no FFmpeg-free decoder; DVB and ID3 timed
   metadata in TS are ignored. Where TS timestamps start over, a jump
   ahead, or a run shorter than a sixteenth of the input, is found only
   when played: until then the duration and seeks miss it.
3. **Windows**: no software video decoder, so a machine without a GPU (or
   a codec its GPU lacks) plays no video; Media Foundation's software mode
   would cover it. AAC in ADTS and other audio codecs have no decoder,
   and http needs item 1. Packaging the FFmpeg-free core for the plugin
   is not started.
4. **Android**: http (item 1), an FFmpeg-free AAR variant if native
   Android users want one, and the Flutter plugin on it.
5. **Fuzzing the live HLS reloads**: they wait real time, so the `hls`
   target makes playlists VOD; it needs a clock the fuzzer can advance.

## Distribution and infrastructure

1. **1.0.0**, once the beta has been used. Before it, and only then, a
   fresh app on `lychee_player` from pub.dev on every platform plays a
   file; betas need no check after publishing, since `release.yml`'s
   `tool/release.sh check` builds fresh apps against the same package
   and assets (the user, 2026-10-27).
2. **Release leftovers**: the AAR on Maven Central too (what native
   Android users expect; needs a Sonatype account for `tech.soit`);
   `tool/release.sh check` building Windows and Linux apps (it runs on
   macOS only); the Linux job in a `ubuntu:22.04` container once GitHub
   retires that
   image.
3. **Native SDKs**: drop the pod and the plugin's copy of
   `LycheeFrameDriver` (`LycheeTexture`) once Flutter drops CocoaPods;
   the Rust crates on crates.io, and built for Windows, Linux, Android
   and iOS (`LYCHEE_LIB_DIR` is untried). Windows and Linux native users
   take the C ABI as it is.
4. **Engineering practices**, in rough order of value:
   - **clang-tidy**: the Android, Windows and Linux backends are not
     checked. Add Linux to the changed-lines CI check, after validating
     its compiler database and diagnostics on Linux.
   - **MSVC `/W4`** for the Windows build, and warnings as errors in the
     plugins' and the Android SDK's own core builds.
   - **CI jobs for Android and Windows**: the SDK build and its unit tests
     on an emulator; the core and `examples/glfw` on `windows-latest`.
   - **Frame checks** by pixels for the Swift and Android examples, as
     `examples/glfw --snapshot` has.
   - **ABI check on other ABIs**: `tool/abi_check.py` probes the host's
     layout only, so a layout that differs only on 32-bit or Windows goes
     unseen.
   - **Performance benchmarks**: decode and frame-conversion throughput,
     seek latency and memory per resolution on the virtual clock, kept
     per commit.
   - **Soak tests**: hours of looping, seeking and file switching on each
     platform's harness, checking memory and thread counts stay flat.
   - **Supply chain**: Dependabot (or Renovate) for the pinned Actions,
     GLFW and FFmpeg sources; SHA-pinned Actions; build provenance
     attestations and an SBOM for release assets.
   - **Fuzzing corpus**: merge it with libFuzzer's `-merge=1` if it grows
     past a few hundred MB; OSS-Fuzz once the project is public.
   - **Coverage** as a CI artifact once someone watches it.
