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
   the BT.709 matrix for untagged HD on a phone's compositor (the
   emulator's converts with BT.601 whatever the dataspace: check
   `h264_aac.mp4` with the example's `snapshot` extra and
   `tool/frame_check.py` without `--matrix`), AAudio route changes to a USB-C headset, and switching AirPods back to
   the speaker after clock correction (waiting on the user and the Xiaomi
   14: the prior run jumped ~0.2 s and dropped 8 frames; verify the step is
   smaller than a frame). Only the CLI's
   `background` case exercises the expiry of
   frames nobody pulled: Choreographer kept pulling in the background on
   the Xiaomi 14.
2. **Windows**: after a real GPU reset (`dxcap -forcetdr`, 2026-10-28)
   the CLI and `lychee_glfw` recover: ~3-4 s without video, the NVIDIA
   HDMI audio endpoint goes away so WASAPI reopens, then ~5 s of audio
   glitches. The Flutter example does not: the engine's ANGLE context is
   lost (`EGL Error: Context Lost (12302)`) and it draws nothing until it
   exits; that is Flutter's engine, not the plugin. An adapter change
   works in the core (`lychee_glfw --adapter 3:1`, Intel to NVIDIA, frames
   pass `frame_check`), but the plugin sets the adapter once, and a window
   moved to another GPU's display needs a monitor on the NVIDIA card
   (hands). HEVC without the Store's HEVC Video Extensions (should fall
   back to D3D11VA at open) waits on the user: the PC has the free "from
   Device Manufacturer" package, which neither winget nor the HK Store
   offers again (only the paid 9NMZLZ57R3T7), so it was not uninstalled.
   Also multiple monitors, and audible output.
3. **Linux**: VAAPI on AMD, Wayland, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still).
4. **Apple without FFmpeg**: AV1 on an M3 or A17 Pro, and VP9 on a
   device with iOS 26.2 or later (waiting on the user): the iOS 26.5
   simulator has no VP9 decoder (`VTDecompressionSessionCreate` -12906,
   2026-10-28).

5. **macOS Swift example SD snapshots**: with the FFmpeg-free core,
   ScreenCaptureKit snapshots after HLS switches to 640x360 differ from
   `tool/frame_check.py`'s low-variant reference (17.6 dB, mean error
   about 20, 2026-10-28). The same frames through macOS GLFW pass at
   30.7-31.9 dB, and the iOS Swift view passes at 33 dB. A static low
   variant plays but its attempted captures failed, so this does not
   establish an adaptation regression. Isolate Core Animation's color
   matching from ScreenCaptureKit's capture conversion; done when both
   Swift snapshots match the low variant's pixels, with advancing pts.

## Known defects

- **The Flutter example crashed on quitting on Windows**: after
  `--seconds N` it calls `exitApplication(AppExitType.required)`, and
  every run on 2026-10-28 between 11:07 and 12:22 ended with 0xC000041D
  (an access violation at `flutter_windows.dll`+0x1e230, Event Log)
  instead of 0, even when no file was opened (Flutter 3.47.7, Impeller
  OpenGL ES). After the PC rebooted, the same build exits with 0 in six
  runs, with a file and without, and so does a bare `flutter create` app
  quitting the same way; with no file the plugin only registers and
  unregisters. What the PC's state was before the reboot is unknown.
  If it comes back, compare the bare app on the same boot before
  touching the plugin.

1. **FFmpeg's HLS fallback loses WebVTT cues**: on Linux,
   `http-hls-subtitles-fallback` advertises the subtitle track as selected
   but never reports its "Hello" cue, with FFmpeg 9.0.2 both before and
   after the subtitle-buffer patch. The shared HLS parser's normal
   `http-hls-subtitles` case reports it correctly. Check packet and cue
   timestamps in the fallback; done when that case also asserts the cue
   appears and clears at the right times.
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
   FFmpeg's hardware decoders (VAAPI, D3D11VA), which get only its
   `low_delay`; what MediaCodec's `KEY_LOW_LATENCY` saves on a phone (the
   emulator's codecs ignore it). After a stall, audio moves up to 500 ms
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

## FFmpeg-free builds

The goal is FFmpeg-free builds on Apple platforms, Windows and Android;
Linux keeps FFmpeg (no system audio decoder or demuxer). Apple plays MP4,
Matroska/WebM, MPEG-TS and HLS without it, and ships
`lychee_noffmpeg.xcframework`; Windows plays the same containers (H.264
and HEVC in hardware or software, AAC, MP3, FLAC, ALAC and Opus), and Android too (video
H.264, HEVC, VP8, VP9 and AV1), each reading http(s) through the system (WinHTTP,
`HttpURLConnection`).

1. **Containers**: IMSC image-profile subtitles and their auxiliary image resources
   are not decoded (text-profile IMSC and WebVTT are read from fMP4). DVB and ID3 timed
   metadata in TS are ignored (FFmpeg's build drops them too): the C ABI
   has no channel for timed data, which would take a track type, an
   event and an acquire call for its bytes at the clock's time (like
   subtitles), in every binding; worth it once a caller needs it (HLS
   ad markers, live captions' ID3).
2. **Windows**: VP9 and AV1 have no decoder (Media Foundation has them
   only with Store extensions), and
   FLAC in MP4 none either (Windows' decoder refuses it, 0xC00D36E6, with
   the STREAMINFO Windows' own MP4 source passes; FLAC in Matroska
   plays). Live
   HEVC decoded in software shows one frame less than it should
   (`NO_FFMPEG=1 NO_HW=1 ONLY=live-hevc`: 299 of 300; the HEVC extension's
   decoder holds the last frame back, low latency mode set or not).
   Draining it after each input that gave no frame loses its reference
   frames (only the first frame shows), so it needs a drain only once the
   input pauses.
3. **Android**: an FFmpeg-free AAR in the release
   (`-Plychee.ffmpeg=false` builds it, and the Flutter plugin plays on
   it; a second artifact is a publishing decision) if native Android
   users want one. VP8, VP9 and AV1 through a phone's hardware MediaCodec
   are unseen (the emulator has VP8 and VP9, and AV1 only in software).
   AC-3 decoding needs an AC-3-capable phone or TV (waiting on the user):
   Pixel_10 API 37 has no `audio/ac3` decoder; disabling that audio track
   while video continues is verified. Play `ac3.ts` or `ac3_audio.mp4`
   on a device that advertises that decoder and confirm audible output.
## Distribution and infrastructure

1. **1.0.0**, once the beta has been used. Before it, and only then, a
   fresh app on `lychee_player` from pub.dev on every platform plays a
   file; betas need no check after publishing, since `release.yml`'s
   `tool/release.sh check` builds fresh apps against the same package
   and assets (the user, 2026-10-27).
2. **Release leftovers**: the AAR on Maven Central too (what native
   Android users expect; needs a Sonatype account for `tech.soit`);
   `release.yml` running `tool/release.sh check` on Linux, where it
   builds Linux apps by hand only (`release.yml` runs the macOS and
   Windows checks, the attestations and the SBOM since 2026-10-28, not
   yet in a release); the Linux job in a `ubuntu:22.04` container once GitHub
   retires that
   image.
3. **Native SDKs**: drop the pod and the plugin's copy of
   `LycheeFrameDriver` (`LycheeTexture`) once Flutter drops CocoaPods;
   the Rust crates on crates.io.
   Windows and Linux native users
   take the C ABI as it is.
4. **Engineering practices**, in rough order of value:
   - **FFmpeg upstream report** (waiting on the user to submit; no report
     has been sent). Suggested report text: "FFmpeg 9.0.2's HLS demuxer
     leaks 34,848 bytes for a WebVTT subtitle rendition. On Linux,
     `ASAN=1 NO_HW=1 ONLY=http-hls-subtitles-fallback FFMPEG_ROOT=UNPATCHED tool/regression.sh`
     in lychee_player reproduces it. In
     `libavformat/hls.c`, `hls_read_header` allocates an unused 32 KB
     `read_buffer` before installing the subtitle's dummy WEBVTT buffer;
     `init_subtitle_context` then overwrites the 2,080-byte AVIO probe
     buffer (and subsequent segment buffers) without freeing it.
     Allocate only the buffer installed in AVIO, and free its current
     buffer before initializing a new subtitle segment. The proposed
     patch is `tool/patches/ffmpeg-hls-subtitle-buffers.patch`."
   - **clang-tidy**: the Android and Windows backends are not checked.
     `tool/tidy.sh` on Linux checks the Linux backend (clang 23, `--all`
     without findings, 2026-10-28). `ci.yml`'s `tidy` job runs on Linux
     too since 2026-10-28 (PyPI's clang-tidy 23, a clang compile
     database against the LGPL FFmpeg), unverified: it has not run on
     GitHub, and `tool/tidy.sh` has not run with that wheel on Linux;
     fix what the first push shows.
   - **CI jobs for Android and Windows** (`android`, `core-windows` in
     `ci.yml`, 2026-10-28) have not run on GitHub yet: fix what their
     first push shows (the emulator's MediaCodec snapshots, WASAPI on a
     VM without an audio device), then delete this line.
   - **Performance benchmarks**: `tool/bench.sh` and
     `tool/bench_compare.py` run by hand on an idle machine. Per commit
     needs an idle runner: hosted VMs are too loaded to compare, and the
     self-hosted Mac was dropped (the user, 2026-10-28).
     VideoToolbox through FFmpeg decodes 1080p H.264 at ~210 fps on the
     virtual clock against software's ~970 (M-series, 2026-10-28): one
     frame at a time, which is enough to play but not to scrub fast.
   - **Soak tests**: `tool/soak.sh` soaks the core through the CLI on
     macOS, Linux and Windows, and `examples/glfw --soak` the Windows
     harness. Left: soaking the other platform harnesses
     (`examples/glfw` on macOS and Linux, the Swift and Android
     examples), whose frame hand-off and display link the CLI skips.
   - **Supply chain**: `updates.yml` (weekly, 2026-10-28) lists newer
     FFmpeg, Mbed TLS and GLFW releases than the pins
     (`tool/check_updates.py`); it has not run on GitHub yet: look at
     its first run, then delete this line.
   - **Fuzzing corpus**: merge it with libFuzzer's `-merge=1` if it grows
     past a few hundred MB; OSS-Fuzz once the project is public.
   - **Coverage** as a CI artifact once someone watches it.
