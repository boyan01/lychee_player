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
   `tool/regression.sh`'s realtime cases on Windows: `realtime-rate`
   once showed 400 ms of drift and `realtime-seek` 6 glitches while an
   Android emulator loaded the PC (2026-10-28); alone they pass. Rerun
   them on an idle PC.
3. **Linux**: VAAPI on AMD, Wayland, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still).
4. **Apple without FFmpeg**: AV1 on an M3 or A17 Pro, and VP9 on a
   device with iOS 26.2 or later (waiting on the user): the iOS 26.5
   simulator has no VP9 decoder (`VTDecompressionSessionCreate` -12906,
   2026-10-28).

## Known defects

- **The Flutter example crashes on quitting on Windows**: after
  `--seconds N` it calls `exitApplication(AppExitType.required)`, and the
  process ends with 0xC000041D (an access violation in
  `flutter_windows.dll`, Event Log) instead of 0, even when no file was
  opened (Flutter 3.47.7, Impeller OpenGL ES, 2026-10-28). The `summary:`
  line comes first, so runs are still readable. Find whether the plugin
  takes part (a bare `flutter create` app quitting the same way) and fix
  or report it upstream; done when the example exits with 0.

1. **MPEG-TS and HLS through FFmpeg's demuxer off Apple**: on Linux,
   Windows and Android the FFmpeg build still reads TS and HLS with
   `FFmpegDemuxer`, so timestamps that start over inside a TS file
   (`ts_timestamp_reset.ts`: 3.021 s instead of 6 s, seeks into the
   wrong half), HLS audio renditions on another timeline
   (`hls_alt/master.m3u8`, 1 s out of sync) and slow seeks past an HLS
   discontinuity remain there; Apple's FFmpeg build opens them with the
   own demuxers (`media/factory.cc`). TS needs only files there; HLS
   reads through the http `DataSource`, which Windows and Android now
   have. TS video
   comes without extradata (Annex B, parameter sets in band), which
   MediaCodec and Media Foundation have not been tried with. Done when
   `tool/regression.sh`'s `own_ts` cases pass on Linux and Windows, and
   the Android example plays `ts_timestamp_reset.ts` with hardware
   decoding.
2. **TS runs found while reading, audio first**: where timestamps start
   over in a part of the input the open did not sample, and a run's
   audio arrives before its video (video PES packets of unknown length
   are handed out only when the next starts), that audio is placed on a
   provisional shift until the video sets the run's shift, then steps
   back by the difference (tens of ms). Seen with a synthetic stream in
   `mp2t_test.cc`, not with a real file.
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
4. **Software AV1**: the shipped FFmpeg has no AV1 software decoder, so
   AV1 plays only where the GPU decodes it (on Apple, M3 and A17 Pro and
   later). Add dav1d (BSD) to `tool/build_ffmpeg.sh` if it matters.

## FFmpeg-free builds

The goal is FFmpeg-free builds on Apple platforms, Windows and Android;
Linux keeps FFmpeg (no system audio decoder or demuxer). Apple plays MP4,
Matroska/WebM, MPEG-TS and HLS without it, and ships
`lychee_noffmpeg.xcframework`; Windows plays the same containers (H.264
and HEVC in hardware or software, AAC and MP3), and Android too (video
H.264 and HEVC only), each reading http(s) through the system (WinHTTP,
`HttpURLConnection`).

1. **Connection reuse**: one `NSURLSession` per demuxer would let HLS
   segments reuse connections.
2. **Containers**: HLS switches variants only when a track is selected,
   not by measured bandwidth; subtitle renditions (WebVTT) are not
   read. AC-3 has
   no FFmpeg-free decoder; DVB and ID3 timed
   metadata in TS are ignored. Where TS timestamps start over, a jump
   ahead, or a run shorter than a sixteenth of the input, is found only
   when played: until then the duration and seeks miss it.
3. **Windows**: VP9, AV1, Opus, FLAC and ALAC have no decoder (Media
   Foundation has them only with Store extensions, or not at all). Live
   HEVC decoded in software shows one frame less than it should
   (`NO_FFMPEG=1 NO_HW=1 ONLY=live-hevc`: 299 of 300; the HEVC extension's
   decoder holds the last frame back, low latency mode set or not).
4. **Android**: VP8, VP9 and AV1 through MediaCodec
   (`MediaCodecVideoDecoder` maps only H.264 and HEVC, so WebM's video
   has no decoder); an FFmpeg-free AAR in the release
   (`-Plychee.ffmpeg=false` builds it; a second artifact is a publishing
   decision) if native Android users want one, and the Flutter plugin on
   it, which is untried.
## Distribution and infrastructure

1. **1.0.0**, once the beta has been used. Before it, and only then, a
   fresh app on `lychee_player` from pub.dev on every platform plays a
   file; betas need no check after publishing, since `release.yml`'s
   `tool/release.sh check` builds fresh apps against the same package
   and assets (the user, 2026-10-27).
2. **Release leftovers**: the AAR on Maven Central too (what native
   Android users expect; needs a Sonatype account for `tech.soit`);
   `release.yml` running `tool/release.sh check` on Windows (Git Bash)
   and Linux, where it builds Windows and Linux apps by hand only; the
   Linux job in a `ubuntu:22.04` container once GitHub
   retires that
   image.
3. **Native SDKs**: drop the pod and the plugin's copy of
   `LycheeFrameDriver` (`LycheeTexture`) once Flutter drops CocoaPods;
   the Rust crates on crates.io.
   Windows and Linux native users
   take the C ABI as it is.
4. **Engineering practices**, in rough order of value:
   - **clang-tidy**: the Android and Windows backends are not checked.
     `tool/tidy.sh` on Linux checks the Linux backend (clang 23, `--all`
     without findings); CI's changed-lines check should run it there too.
   - **CI jobs for Android and Windows**: the SDK build and its unit tests
     on an emulator; the core and `examples/glfw` on `windows-latest`.
   - **Performance benchmarks**: `tool/bench.sh` measures them on one
     machine, by hand; left are running it per commit on the idle
     self-hosted Mac and keeping the table somewhere CI can compare.
     VideoToolbox through FFmpeg decodes 1080p H.264 at ~210 fps on the
     virtual clock against software's ~970 (M-series, 2026-10-28): one
     frame at a time, which is enough to play but not to scrub fast.
   - **Soak tests**: `tool/soak.sh` soaks the core through the CLI on
     macOS and Linux. Left: a run on Linux and Windows, and soaking the
     platform harnesses (`examples/glfw`, the Swift and Android
     examples), whose frame hand-off and display link the CLI skips.
   - **Supply chain**: build provenance attestations and an SBOM for
     release assets; updates for the FFmpeg, GLFW and Mbed TLS sources
     pinned in `tool/` scripts, which Dependabot cannot read.
   - **Fuzzing corpus**: merge it with libFuzzer's `-merge=1` if it grows
     past a few hundred MB; OSS-Fuzz once the project is public.
   - **Coverage** as a CI artifact once someone watches it.
