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
   AAudio route changes to a USB-C headset (deferred at the user's
   request), and a sub-frame timing check
   when switching AirPods back to the speaker after clock correction.
   On the Xiaomi 14 (2026-10-29), turning Bluetooth off restored speaker
   output and the user noticed no stutter, but two captured switches
   added 8 and 3 dropped frames; the count then stayed flat. The example's
   500 ms position samples cannot establish that the clock step is
   smaller than a frame; that check needs finer timing instrumentation
   before another device run. Only the CLI's
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
3. **Linux**: VAAPI on AMD, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still). HDR dmabuf imports through both GLFW and Flutter wait
   on the user to boot the PC into Ubuntu 24.04 and log in to X11.
   Play `hdr_pq_colors.mp4` and `hdr_pq_highlights.mp4` through Intel VAAPI
   and check two advancing snapshots from each consumer with the existing
   `tool/frame_check.py` thresholds.
4. **Apple without FFmpeg**: AV1 on an M3 or A17 Pro, and VP9 on a
   device with iOS 26.2 or later (waiting on the user): the iOS 26.5
   simulator has no VP9 decoder (`VTDecompressionSessionCreate` -12906,
   2026-10-28).

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

1. **iOS foreground recovery needs device verification** (waiting on the
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

1. **Live input in flutter-airplay** (waiting on the user): enable the
   experimental local SDK build (`AIRPLAY_LYCHEE_ROOT`, flutter-airplay's
   DEVELOPMENT.md) on macOS and Android. Mirror an iPhone with moving
   video and speech while Bluetooth headphones are connected; confirm A/V
   remains in sync, including after a short network stall and after
   disconnect/reconnect. On a phone that advertises MediaCodec low latency,
   compare the same stream with `KEY_LOW_LATENCY` enabled and disabled;
   the emulator's codecs ignore it. After a stall, the current timeline
   allows audio up to 500 ms behind video and returns over a few seconds.
   Done when the real mirroring and Bluetooth checks pass on both hosts,
   and the phone's low-latency result is recorded.
2. **HDR beyond Apple's SDK**:
   - Flutter: textures are 8-bit SDR on every platform, so the plugin
     stays on tone-mapped BGRA.
   - Android: the user confirmed incorrect HDR colors on the Xiaomi 14
     with `hdr_pq_colors.mp4` through Flutter's MediaCodec into a
     `SurfaceProducer` (2026-10-29). The decoder still reports BT.2020/PQ
     output. Investigate requesting SDR (`color-transfer-request`, API 31)
     for the plugin's surface only, behind a `lychee_config` flag; the
     SDK's SurfaceView must keep HDR. Done when Flutter's SDR output has
     correct colors on the HDR phone and the user confirms the display.
   - Apple: VideoToolbox's tone mapping of bright HDR colors differs from
     swscale's reference (`hdr_pq_bars.mp4`: 27.9 dB), and HLG comes out
     darker (98 for a 0.5 signal against 142); that is Apple's mapping.

## FFmpeg-free builds

The goal is FFmpeg-free builds on Apple platforms, Windows and Android;
Linux keeps FFmpeg (no system audio decoder or demuxer). Apple plays MP4,
Matroska/WebM, MPEG-TS and HLS without it, and ships
`lychee_noffmpeg.xcframework`; Windows plays the same containers (H.264,
HEVC, VP9 and AV1 in hardware or software, the latter three with their
Store extensions; AAC, MP3, FLAC, ALAC and Opus), and Android too (video
H.264, HEVC, VP8, VP9 and AV1), each reading http(s) through the system (WinHTTP,
`HttpURLConnection`).

1. **Containers**: DVB and ID3 timed
   metadata in TS are ignored (FFmpeg's build drops them too): the C ABI
   has no channel for timed data, which would take a track type, an
   event and an acquire call for its bytes at the clock's time (like
   subtitles), in every binding; worth it once a caller needs it (HLS
   ad markers, live captions' ID3).
2. **Android**: an FFmpeg-free AAR in the release
   (`-Plychee.ffmpeg=false` builds it, and the Flutter plugin plays on
   it; a second artifact is a publishing decision) if native Android
   users want one.
   VP8 hardware decoding needs another phone: the Xiaomi 14 exposes only
   the software VP8 path, so it cannot close that hardware check.
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
   the Linux jobs in `ubuntu:22.04` containers once GitHub retires those
   runner images.
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
   - **Performance benchmarks**: `tool/bench.sh` and
     `tool/bench_compare.py` run by hand on an idle machine. Per commit
     needs an idle runner: hosted VMs are too loaded to compare, and the
     self-hosted Mac was dropped (the user, 2026-10-28).
   - **Fuzzing corpus**: merge it with libFuzzer's `-merge=1` if it grows
     past a few hundred MB; OSS-Fuzz once the project is public.
   - **Coverage** as a CI artifact once someone watches it.
