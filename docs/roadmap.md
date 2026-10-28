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
   D3D11VA at open), multiple monitors, and audible output.
   `tool/regression.sh`'s realtime cases on Windows: `realtime-rate`
   once showed 400 ms of drift and `realtime-seek` 6 glitches while an
   Android emulator loaded the PC (2026-10-28); alone they pass. Rerun
   them on an idle PC.
3. **Linux**: VAAPI on AMD, Wayland, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still).
4. **Apple without FFmpeg**: AV1 on an M3 or A17 Pro, VP9 on iOS 26.2 or
   later (no runtime here), A/V sync of Opus and MP3 against a reference
   clip.

## Known defects

- **The Flutter example crashes on quitting on Windows**: after
  `--seconds N` it calls `exitApplication(AppExitType.required)`, and the
  process ends with 0xC000041D (an access violation in
  `flutter_windows.dll`, Event Log) instead of 0, even when no file was
  opened (Flutter 3.47.7, Impeller OpenGL ES, 2026-10-28). The `summary:`
  line comes first, so runs are still readable. Find whether the plugin
  takes part (a bare `flutter create` app quitting the same way) and fix
  or report it upstream; done when the example exits with 0.

- **Apple framework deployment warning**: `pixel_buffer.cc` calls
  `CVBufferCopyAttachment`, available on macOS 12+, while the framework
  targets macOS 11. Both macOS slices warn during `tool/build_apple.sh`.
  `NO_FFMPEG=1` also warns that `allow_hardware` in `factory.cc` is
  unused on iOS. Add an availability-safe attachment read while retaining
  the supported minimum, and account for the iOS argument; done when all
  Apple slices build without these warnings.

- **Timestamps restarting inside a single TS file without FFmpeg**:
  `tool/regression.sh` generates `ts_timestamp_reset.ts` by joining two
  three-second TS files without remuxing. Reproduce with
  `build-noffmpeg/examples/cli/lychee_cli --virtual-time
  /tmp/lychee-media/ts_timestamp_reset.ts`: only the first 90 video frames
  play, the second 90 are dropped, and the reported duration is 3.029 s
  instead of about 6 s. `TsReader` unwraps 33-bit timestamps but does not
  join resets; `TsDemuxer` estimates duration and bisects seeks using the
  raw timeline. Done when both segments play in sync and seeks work on
  either side of the reset, without scanning an entire large TS at open.
  Keep the failing playback out of the default regression matrix until
  the timeline and seek handling are fixed.

1. **VideoToolbox gives up on damaged TS**: on TS that lost one packet in
   a thousand (`ts_lost_packets.ts`) `VTVideoDecoder` reaches its
   consecutive-error limit, where FFmpeg's software decoder conceals and
   plays on, so the FFmpeg-free build plays `ts-lost-packets` audio only.
2. **Slow HLS seeks past a discontinuity with FFmpeg**: FFmpeg's HLS seek
   drops every packet past a discontinuity where timestamps start over,
   so `FFmpegDemuxer` reads from before the first discontinuity (from the
   start if none has been read yet). Long streams make that slow; only an
   HLS demuxer that knows the segments, as the FFmpeg-free one does,
   would do better.
3. **iOS foreground recovery needs device verification** (waiting on the
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
   - Windows: PQ and HLG are tone-mapped by our shader like swscale
     (saturated colors at or above white approximate it: swscale clips in
     IPT, the shader in ICtCp). Left: HDR output, which needs an
     R10G10B10A2 or FP16 texture type.
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
`lychee_noffmpeg.xcframework`; Windows plays the same containers (H.264
and HEVC in hardware or software, AAC and MP3); Android plays MP4 files.

1. **Shared IO on Android**: an http `DataSource` there
   (`HttpURLConnection` or Cronet through JNI), which its FFmpeg-free
   build needs anyway; then FFmpeg reads through it as on Apple and
   Windows (WinHTTP). One `NSURLSession` per demuxer would let HLS segments reuse
   connections.
2. **Containers**: HLS with `EXT-X-MAP` (fMP4 segments: a `DataSource`
   that joins the init segment to each media segment, opened by
   `Mp4Demuxer`), AES-128 (CommonCrypto, CBC per segment, the key fetched
   once), `EXT-X-MEDIA` audio renditions and variant switching. MP3 in TS
   is untested; AC-3 has no FFmpeg-free decoder; DVB and ID3 timed
   metadata in TS are ignored.
3. **Windows**: VP9, AV1, Opus, FLAC and ALAC have no decoder (Media
   Foundation has them only with Store extensions, or not at all). Live
   HEVC decoded in software shows one frame less than it should
   (`NO_FFMPEG=1 NO_HW=1 ONLY=live-hevc`: 299 of 300; the HEVC extension's
   decoder holds the last frame back, low latency mode set or not).
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
   `tool/release.sh check` building Linux apps (it builds macOS, iOS and
   Android apps on macOS and Windows apps in Git Bash on Windows, which
   `release.yml` does not run yet); the Linux job in a `ubuntu:22.04` container once GitHub
   retires that
   image.
3. **Native SDKs**: drop the pod and the plugin's copy of
   `LycheeFrameDriver` (`LycheeTexture`) once Flutter drops CocoaPods;
   the Rust crates on crates.io, and built for Linux, Android and iOS
   (`LYCHEE_LIB_DIR` has run on Windows only). Windows and Linux native users
   take the C ABI as it is.
4. **Engineering practices**, in rough order of value:
   - **clang-tidy**: the Android, Windows and Linux backends are not
     checked. Add Linux to the changed-lines CI check, after validating
     its compiler database and diagnostics on Linux.
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
