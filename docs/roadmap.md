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
   exits; that is Flutter's engine, not the plugin. HEVC without the Store's HEVC Video Extensions (should fall
   back to D3D11VA at open) waits on the user: the PC has the free "from
   Device Manufacturer" package, which neither winget nor the HK Store
   offers again (only the paid 9NMZLZ57R3T7), so it was not uninstalled.
   Also audible output. FFmpeg's D3D11VA decoding has not run since
   output textures are pooled (2026-11-02): Media Foundation takes every
   file the examples play; it needs a stream MF refuses and D3D11VA
   decodes, checked with `lychee_glfw --snapshot` and `frame_check.py`.
   ASF/WMV has no decoder but the system's, and on CI's Windows Server
   it opens and decodes nothing (`video decoding failed`), so CI skips
   `asf-wmv2` (`NATIVE_WMV=0`); it played on Windows 11 Pro (2026-11-07).
   Windows N and Server users get that same error.
3. **Linux**: VAAPI on AMD, hardware decoding on arm64, and audio
   on a real sound card (WSLg's `RDPSink` runs 0.3-0.4% slow, so video
   holds a frame every ~3 s to follow it; an arm64 VMware VM's card plays
   slower still).
4. **HLS prefetch on Android and Linux** (2026-11-02): their sources
   connect on their first read, so a thread now reads the next segment
   ahead (`CreatePrefetchingDataSource`); its unit tests show it reading
   ahead, but no run has shown the download overlapping the current
   segment there. Done when `media_server.py --verbose`'s log of
   `http-hls-prefetch-hls` on Linux, or of the Android example playing
   `hls/index.m3u8`, has segment 5 requested before segment 4's last
   bytes went out.
5. **iOS audio after the background**: on the iPhone 17 (2026-10-29)
   sound and hardware video came back after the Home Screen, but that
   run's audio device never failed to start, so the renderer's retry of
   a refused start (`'what'`, `audio device started on retry`) is seen
   only in `audio-device-start-retry`. Also unseen: an alarm or call
   alone (an audio interruption) recovering without the foreground hook.

## Known defects

- **macOS MP3 fuzz run leak report** (2026-11-07): `FUZZ_TARGET=mp3
  tool/fuzz.sh` completed 116,883 inputs without a crash artifact, but
  LeakSanitizer reported 995 bytes allocated by the sanitizer's symbolizer
  while libFuzzer printed coverage. Determine whether the toolchain leak
  is reproducible and make the run exit cleanly without hiding player leaks.

- **Flutter example macOS profile linker warning** (2026-11-06): after
  adding `desktop_drop` 0.9.2 and the `cross_file` 0.4 file-selector
  migration, Xcode's linker warns that an address "points before section"
  and "the target atom is ambiguous". The app builds and plays; the input
  object causing the warning is unknown. Done when it is identified and
  a profile link completes without this diagnostic.

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
- **MPEG-TS of identical short clips joined**: runs that start over at
  the same timestamp within 256 KB of each other are taken for one
  (`TsDemuxer::AddRun` matches by offset and start time), so 12 MB of a
  2-frame clip repeated opens with a duration of 66 ms (found
  2026-11-03). Done when such a file's duration and seeks cover every
  run, as a regression case shows.
- **Raw elementary streams through FFmpeg** (`.h264`, `.hevc`, `.vvc`
  from the FATE suite, Windows, 2026-11-04): they open with no duration,
  decode nothing through Media Foundation, and a seek ends playback with
  "seek failed" instead of being ignored. Done when such a file plays and
  an unseekable seek leaves it playing.
- **HEVC, and H.264 with POC type 1, without composition offsets**:
  MP4s with B-frames and no `ctts` get display times from the pictures
  (`DecodeOrderTimestamps`) only for H.264 with POC type 0 or 2; the
  others keep decode times and jump back and forth like H.264 did. None
  has been seen. Done when an HEVC `tool/mp4_ctts_case.py` file passes a
  `frames` case.
- **Seen once in the 2026-11-04 Windows sweep, unchecked**: FATE's
  `cover_art.flac` ends at 0.186 s with FFmpeg's "invalid residual";
  Chromium's `bear-1280x720-av_frag.mp4` ends early without FFmpeg and
  with "audio decoding failed" with it. Done when each is explained or
  plays to its end.

- **Live HLS through FFmpeg does not seek** (2026-11-09): the FFmpeg
  fallback (variants the native HLS reader cannot decode, SAMPLE-AES,
  subtitle renditions only FFmpeg reads) refuses seeks in a live
  playlist, so such input reports no seekable range. Done when a
  `http-hls-live-window` case through that fallback seeks back and
  returns to the edge.
- **Paused behind live past the window** (2026-11-09): live HLS paused
  (or seeked back) longer than a sliding playlist keeps its segments
  fails when the next segment has left the playlist, instead of moving
  on to the window's start. Done when a regression case pauses past the
  window and plays on from its start.

- **Android Surface snapshots**: `LycheePlayerView.snapshot()` uses
  PixelCopy, whose padded-YUV Surface readback applies a crop/scale that
  differs from the screen compositor. Actual MPEG-2 output and compositor
  captures pass the measured-geometry/sRGB checks; producer Surface
  snapshots still inherit the platform limitation. Done when a public-API
  snapshot path preserves the displayed pixels for padded YUV without a
  fitted crop correction.

## Performance

What the review of 2026-11-02 found and left, by expected gain.

1. **Linux VAAPI frames** (waiting on the user to boot the dual-boot
   Ubuntu at 192.168.31.212 and log in): `WrapDmabuf` maps every frame
   (`av_hwframe_map`, new dmabuf fds), and the Flutter plugin and
   `examples/glfw` create and destroy two EGLImages per frame. Cache
   them by the fds' `st_dev`/`st_ino`, offset, pitch, modifier, fourcc
   and size, cleared on a size or format change and holding no more than
   the decoder's surface pool. Done when both pass `frame_check.py` there
   on iHD and the raster time per frame drops.
2. **Flutter on Apple converts every frame to BGRA**: Flutter 3.47.8's
   Darwin external textures decode 420v/420f as BT.601 whatever the
   buffer's tags, so HD would come out wrong as NV12. Done when the
   engine reads the tags (or the plugin converts in a shader of its own)
   and the plugin hands 8-bit SDR over as NV12, checked by
   `frame_check.py` on `h264_bt601.mp4` and an HD file.
3. **Android software frames** are copied into the window, ~33 MB a
   frame at 4K. Done when the core hands out a buffer the compositor
   takes (AHardwareBuffer with ASurfaceControl, or a YV12 window),
   checked by pixels.
4. **HLS**: AES-128
   segments are still decrypted whole before their first packet
   (streaming CBC needs a chained cipher on CommonCrypto, BCrypt and
   javax.crypto); only one segment is fetched ahead.
5. **Demuxers**: a fragmented MP4 whose last moof has no `tfdt` (Smooth
   Streaming) still reads every fragment at open; bisecting a Matroska
   file without cues scans up to a cluster per step, tens of MB per seek
   with 4K clusters (SimpleBlock times or PrevSize would narrow it).
6. **Legacy container startup**: FLV, AVI and MPEG-PS scan input at open
   to build an index capped at one million packets. They keep payloads in
   the source, but long files and HTTP inputs still pay for the scan.
   Use container indexes where available, with lazy/sparse fallback.
   Done when a long HTTP input starts without scanning to its end while
   retaining accurate seeks and bounded memory.
7. **Locks**: `Player::SetVideoSurface` still calls
   `AMediaCodec_setOutputSurface` under the player's and the renderer's
   locks; D3D11 output textures wait 100 ms after their frame's release
   before reuse, where a keyed mutex or shared fence through the ABI
   would be exact.

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
     fade). Not reproduced offline yet. Since found: the Android, Apple
     and Windows decoders counted audio timestamps from the first packet,
     so after a gap or an audio timeline move nearly all audio played late
     and was dropped (TCL TV, 2026-10-31); they now follow the packets'
     timestamps (`live-audio-gap`). Recheck this session after it.
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

## TV Play gaps

What TV Play (an Android TV and desktop app on the Flutter plugin) works
around today (its `lib/core/playback/lychee_engine.dart` marks each as
`gap #N`, the numbers below). Read from the beta.7 Dart API; none was
reproduced on a device. Each item is finished on every binding it
reaches (C ABI, Flutter, Rust, Swift, Kotlin), like any feature.

2. **Error kinds** (#16, #9): the error carries a code (network, HTTP
   status, decoder, format) besides its message, so an app can tell a 403
   (fetch a new URL) from a dropped connection (retry) from an unsupported
   codec (another quality). Done when regression cases see each code.
   Per-request observation (#9) only if it fits the ABI's plain-callback
   rule cheaply; otherwise say so here.
6. **Source lifecycle** (#5): open the
   next item ahead so it starts without a gap (music queue, next
   episode). Done when regression cases show each, and the gapless one
   by `tool/audio_check.py`.
7. **Android TV**: `content://` URIs (#17) for media and subtitle files,
   opened through `ContentResolver`; display mode matching (#19), the
   output refresh rate following the video's frame rate during playback
   and restored after. The refresh rate switch needs a TV that offers
   several modes: waiting on the user once built.
8. **Separate audio, and DASH** (#2, #3): open a video URL with
   separate audio URLs, which appear as audio tracks; MPEG-DASH
   manifests (`.mpd`: segment templates and lists, several
   Representations and AdaptationSets for audio and subtitles) without
   FFmpeg. Done when regression cases play both, switch the audio
   track and seek.
9. **Thumbnails** (#10): a frame at a given time, decoded apart from
   playback, for seek previews without sprite sheets.
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
2. **Bitmap subtitles**: DVB and VobSub still need FFmpeg. Done when
   `subtitle-dvb` and `subtitle-vobsub` pass with `NO_FFMPEG=1`.
3. **Android**: VP8 hardware decoding needs another phone: the Xiaomi 14
   exposes only the software VP8 path, so it cannot close that hardware check.
4. **Windows**: Media Foundation's Opus MFT after a flush is unchecked:
   `MFAudioDecoder` corrects the first output's time only when less than
   one packet is missing. Seeks now read Opus from 80 ms early, which
   covers a decoder that drops the seek preroll as Android's does.
5. **Windows**: Media Foundation opens some video it then never decodes,
   so playback runs to the end with `decoded=0` and no error: VP9 4:2:2
   and 4:4:4 (with alpha too), HEVC RExt 12-bit and 4:4:4 RGB
   (ProcessInput 0xc00d3e8c), AV1 4:4:4 (FATE suite, Windows 11 26100,
   2026-11-04). Done when `MFVideoDecoder` refuses such configurations at
   open (profile, chroma, bit depth) or fails on the first rejected
   input, so the track reports an error.
6. **Standalone audio metadata and chaining**: the native MP3 reader
   reads ordinary ID3v2.3/v2.4 APIC cover frames, but skips ID3 tags,
   extended headers, unsynchronized or transformed frames, and ID3v2.2 pictures; the Ogg reader
   handles one logical audio stream, not chained Ogg files. Done when
   `artwork-id3` passes without FFmpeg and a chained Opus/Vorbis file
   plays and seeks across its logical-stream boundary.

## Distribution and infrastructure

- **Flutter example file selection**: `desktop_drop` 0.9.2 needs
  `cross_file` 0.4, while the published `file_selector` still needs 0.3.
  The example pins all file-selector packages to the matching migration
  in flutter/packages#13147. Done when that migration is published and the
  example uses pub.dev versions with no Git pins or platform overrides.

1. **1.0.0**, once the beta has been used. Before it, and only then, a
   fresh app on `lychee_player` from pub.dev on every platform plays a
   file. Betas need no fresh-app check, after publishing or in
   `release.yml` (the user, 2026-10-27 and 2026-10-30).
2. **Release leftovers**:
   the Linux jobs in `ubuntu:22.04` containers once GitHub retires those
   runner images.
3. **Native SDKs**: drop the pod and the plugin's copy of
   `LycheeFrameDriver` (`LycheePortedFrameDriver`) once Flutter drops CocoaPods;
   the Rust crates on crates.io (and their badge in the README).
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
