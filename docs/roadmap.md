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
   slower still).

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
- **Looping Opus WebM muxed by FFmpeg leaves a gap**: FFmpeg's Matroska
  muxer writes the segment Duration on the block timeline before
  CodecDelay, rounded to milliseconds (`vp8_opus.webm`: 4.008 s for
  4.000 s of audio and video), and each pass lasts the container's
  duration, so every pass ends with ~8 ms of padding (heard on the
  Xiaomi 14, 2026-10-29; with Duration set to 4000 it is gone). Same
  file, every backend. A fix would take the pass length from where the
  tracks end (last block plus its BlockDuration, less CodecDelay and
  DiscardPadding) when that is shorter. The FFmpeg-free Matroska
  demuxer also ignores DiscardPadding and, with a DefaultDuration, the
  last block's BlockDuration. `lychee_cli --loop --volume 1
  --audio-dump` of `vp9_opus.webm` shows it: `tool/audio_check.py
  --max-gap 1` finds 8.4 ms of silence at 10.000 s (macOS, 2026-10-29).
  Done when that passes as an `audio` case in `tool/regression.sh`.
  `audio_only.mp3` (44.1 kHz) also joins its passes with a small step
  (0.006 at a zero crossing, above `audio_check.py`'s 0.005), where 48
  kHz AAC joins without one.
- **Looping AAC without an edit list leaves a gap**: MPEG-TS and
  Matroska carry no edit list, so AAC's 1024 priming samples play as
  ~21 ms of silence at the start of every pass, with FFmpeg and without
  (`tool/audio_check.py --max-gap 1` after `lychee_cli --loop
  --audio-dump`, 2026-10-29: `h264_aac.mkv` 23 ms, `keyframes.ts` 28
  ms). A fix would drop the codec's delay at a loop's start (FFmpeg's
  `initial_padding`, or 1024 for AAC-LC with no edit list) while the
  first pass keeps the file's timing. `frag.mp4` shows a longer gap
  (14.5 ms with FFmpeg, 54 ms without) for another reason: FFmpeg's
  fragmenting muxer stretched its first audio sample to 66.7 ms, so
  the container's duration is ~45 ms longer than its audio; a test
  file artifact, not worth a case. Done when `h264_aac.mkv` and
  `keyframes.ts` pass as `audio-loop` cases.

1. **iOS foreground recovery can fall back to software**: an iPhone 17
   (2026-10-27) rejected 32 video packets (`-12909`)
   after an apparent interruption or background transition; the actual
   lifecycle trigger is unconfirmed. The native and Flutter frame drivers
   now request a fresh hardware decoder after becoming active following
   a background transition. On the iPhone 17 (2026-10-29), the user saw
   normal picture and sound after five seconds on the Home Screen, but
   the native harness logged repeated VideoToolbox `-12903` failures and
   ultimately fell back to software; HTTP reconnects also produced corrupt
   packet warnings; their causal role is unconfirmed. Investigate the
   hardware fallback from the saved logs. Cover paused-state retention
   and resume with an automated harness, not another manual pause test.
   Done when foreground recovery promptly restores hardware playback.
   Also distinguish an audio interruption from
   an actual background transition: an alarm alone may not enter this hook.
   The simulator cannot reproduce this hardware
   failure; direct C ABI consumers must call
   `lychee_player_rebuild_video_decoder()` on their platform's recovery
   event. Released plugin binaries pinned to API 11 do not have this hook.

## Features

1. **Live input in flutter-airplay** (waiting on the user): build it
   against a local checkout (`AIRPLAY_LYCHEE_ROOT`, flutter-airplay's
   DEVELOPMENT.md) on macOS and Android. Mirror an iPhone with moving
   video and speech while Bluetooth headphones are connected; confirm A/V
   remains in sync, including after a short network stall and after
   disconnect/reconnect. On a phone that advertises MediaCodec low latency,
   compare the same stream with `KEY_LOW_LATENCY` enabled and disabled;
   the emulator's codecs ignore it. After a stall, the current timeline
   allows audio up to 500 ms behind video and returns over a few seconds.
   Done when the real mirroring and Bluetooth checks pass on both hosts,
   and the phone's low-latency result is recorded.
   - Crackle, then silence, on macOS (2026-10-29, iPhone mirroring,
     AAC-ELD): 10 s into a session the RTP stream mixed in no-data
     packets (12 bytes, or the `00 68 34 00` marker), which UxPlay's
     `raop_buffer_dequeue` drops, so lychee got audio with gaps and the
     user heard crackle; then every packet was no-data and nothing reached
     lychee. Suspect how the live audio path cuts or pads those gaps (no
     fade). Not reproduced offline yet.
   - Capture and replay, so a mirroring problem needs no manual A/B run:
     an environment switch in flutter-airplay writes each packet
     `lychee_player_push_live_packet` gets (track, pts, bytes) to a file;
     its `lychee_live_tests` replays such a file on the virtual clock and
     dumps the audio for `tool/audio_check.py`; the live player's periodic
     log adds audio buffered, padded and late-dropped counts. Done when
     the capture of a session like the one above replays to the same
     clicks and is kept as a regression case.
   - flutter-airplay's `lychee-only` branch plays through lychee on all
     five platforms and has no player of its own; it needs a local
     checkout (`AIRPLAY_LYCHEE_ROOT`) on every platform, so its CI and
     release jobs cannot build yet. Done when they take lychee from a
     release (the Apple frameworks, the AAR, the core's source) instead.
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
   Opus seeks lose 80 ms: Android's decoder discards SeekPreRoll after
   a flush, and the demuxer seeks to the target rather than that much
   before it (Chromium seeks back by it). Media Foundation's Opus MFT
   after a flush is unchecked: `MFAudioDecoder` corrects the first
   output's time only when less than one packet is missing.
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
