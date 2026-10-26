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

1. **`realtime-loop` drifts on CI's hosted VM**: it plays on the real
   clock and audio device, so load shows up as drift, and CI's plain job
   failed on it once (2026-10-26). A red plain job (or `core-linux`,
   which runs the same cases) needs its log read.
   Done when CI runs the `realtime=1` cases somewhere their timing means
   something, or not at all. The virtual-time flakes of that day
   (`live-resolution`, `live-jitter`, `subtitle-mid-cue`) are fixed: time
   ran ahead of a slow live decoder, and of events still waiting for the
   control thread's next 10 ms tick; 24 runs six at once now give the
   same counts every run.
2. **Events of players a hot restart left go to a closed isolate**: the
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
3. **A track switch on input that cannot seek loses the read-ahead**: a
   track switch, new video device or decoder rebuild on input that cannot
   seek (`lychee_player_is_seekable()` 0, e.g. an http server without
   range requests through FFmpeg) does not touch the demuxer and goes on
   from its read position, so what the old renderers had queued is
   skipped (2026-10-26). Done when playback there resumes at the position
   on screen (e.g. by replaying the packets still queued), with
   `http-no-ranges-track` checking the first frame after the switch.

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
     still unchecked, waiting on the user's HDR phone. The API 37
     emulator cannot show it (2026-10-26): no codec there takes HEVC
     Main 10 (`c2.goldfish.hevc.decoder` fails at the first packets,
     `c2.android.hevc.decoder` lists only Main and silently drops them),
     so the SDK and the plugin both fall back to FFmpeg and show the
     same swscale tone mapping, pixel for pixel: `hdr_pq.mp4` 181, the
     `hdr_pq_bars.mp4` bars 190, 253 and white from 400 nits. If the
     phone shows the plugin washed out (PQ read as SDR), the cheap fix
     is asking the decoder for SDR (`color-transfer-request`, API 31,
     where the codec can tone-map) for the plugin's surface only, which
     needs a `lychee_config` flag (the SDK's SurfaceView must keep HDR).
     A MediaCodec video decoder that outputs nothing for 64 packets now
     fails, so the next decoder takes over instead of loading forever.
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
   time, with no audio device, plain playback shows none. On a private
   PulseAudio null sink in the same WSL, `realtime-loop` passes
   (2026-10-26, Distribution 4), so the drift is RDPSink's. Not yet seen
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
decoder loss, HDR included) and passes.

Android without FFmpeg plays MP4 files (2026-10-26):
`-Plychee.ffmpeg=false` on the SDK's Gradle build (`-DLYCHEE_WITH_FFMPEG=OFF`)
builds `Mp4Demuxer`, MediaCodec video with the platform's software codecs
as the fallback (`c2.android.*`, reported as `sw`), and
`MediaCodecAudioDecoder` (AAC including ADTS, MP3, Opus, FLAC, ALAC
from Android 13). MediaCodec only decodes, so channels are mixed
(`ChannelMatrix`) and the rate converted (`Resampler`, windowed sinc)
by our own code, unit-tested. `liblychee.so` is then 338 KB and the APK
carries no FFmpeg. On the API 37 emulator the SDK example plays
`h264_aac.mp4` and `rotated.mp4` (hw `c2.goldfish.h264.decoder`, sw
`c2.android.avc.decoder`, upright) with audio driving the clock through
loops; the CLI cross-built for Android plays AAC (44.1 kHz mono to
48 kHz stereo), MP3, Opus and FLAC in MP4 with seeks, drifting no more
than the FFmpeg build there. Decisions:

- No http without FFmpeg on Android: the NDK has no HTTP client, and one
  in C++ (TLS, redirects, proxies) is what FFmpeg plus mbedTLS already
  is. When the FFmpeg-free AAR ships, http should go through Java's
  `HttpURLConnection` via JNI behind `DataSource`, like
  `HttpDataSource` on Apple; until then `open` fails with "no HTTP
  client in this build".
- The shipped AAR keeps FFmpeg, and the Android FFmpeg build keeps
  FFmpeg's audio decoders: `MediaCodecAudioDecoder` and `Resampler` are
  compiled only without FFmpeg.

Apple's decisions:

- Reordering needs no SPS parser: a decoded frame goes out once a packet
  with `dts >= its pts` was decoded (no later frame can come before it),
  the rest at the end of the stream; live input (`low_delay`) outputs at
  once.
- The MP4 parser is our own (~700 lines) rather than Chromium's
  `media/formats/mp4`, which is built around MSE's fragmented MP4.
- The FFmpeg build keeps FFmpeg's demuxer and hwaccel; the new backends
  are compiled only without FFmpeg, so the shipped xcframework does not
  carry dead code.

Both builds read through `DataSource` on Apple platforms (2026-10-26):
the FFmpeg demuxer's `io_open` hands FFmpeg an `AVIOContext` over a
`DataSource` for paths, `file://` and `http(s)://`, the input and what
demuxers open themselves (HLS playlists and segments) alike, so https
goes through NSURLSession (system TLS, proxies, HTTP/2) instead of
FFmpeg's deprecated SecureTransport. Decisions:

- Only on Apple platforms. Android, Windows and Linux have no http
  `DataSource`, so FFmpeg keeps its own protocols there (Android's
  mbedTLS `ca_file` through `io_open` unchanged); reading only files
  through `DataSource` would split one input between two stacks for
  nothing, and `fopen` takes no UTF-8 paths on Windows.
- `HttpDataSource` took over the FFmpeg build's network behaviour:
  reconnects after 0, 1 and 3 s on dropped connections and 5xx (FFmpeg's
  `reconnect_delay_max=4`), not on 4xx; a connection silent for 10 s
  counts as dropped (`rw_timeout`); `Range: bytes=0-` from the first
  request, so a 206 (or `Accept-Ranges`) says the server takes ranges.
- `DataSource` gained three defaulted methods: `seekable()` (FFmpeg
  sets `AVIO_SEEKABLE_NORMAL` from it, so a server without ranges is
  unseekable as with FFmpeg's http, and demuxers do not read the whole
  file to look at its end), `url()` (after redirects: FFmpeg's HLS asks
  for the `location` to resolve relative segment URLs) and
  `cleartext_refused()`. App Transport Security refuses cleartext http
  to host names in apps without an exception; FFmpeg's http, which read
  those URLs before, then reads them instead. The FFmpeg-free build has
  no such fallback: such apps need an ATS exception.
- `data:` and `crypto+http://` (AES-128 HLS) stay with FFmpeg's
  protocols. Each opened URL gets its own `NSURLSession`, so HLS
  segments do not reuse connections; FFmpeg's `http_persistent` is off
  (it asserts on a mix of FFmpeg's and our contexts).
- `Mp4Demuxer` still seeks on a server without ranges (it re-reads from
  the start), where the FFmpeg build reports the input unseekable.

`tool/regression.sh` gained `http-hls` and `http-hls-redirect`.

Matroska and WebM without FFmpeg (2026-10-26): `MatroskaDemuxer`
(`backends/matroska`, our own reading of RFC 9559) over the same
`DataSource`: SimpleBlock and BlockGroup, Xiph, EBML and fixed lacing,
header stripping (other ContentEncodings skip the track), clusters of
unknown size, seeks through the Cues or, without them, an index built by
skipping from cluster header to cluster header; Tags, fonts and cover
art from Attachments; text subtitles (S_TEXT/UTF8, ASS/SSA, WebVTT in
both mappings) through `TextSubtitleDecoder`, which drops markup as
FFmpeg's decoders do. `VTVideoDecoder` adds VP9 (the supplemental decoder,
vpcC from CodecPrivate and Colour, rebuilt when a keyframe header changes
profile, depth or size) and AV1 (av1C); `ATAudioDecoder` adds Opus, FLAC
and MP3. `NO_FFMPEG=1 tool/regression.sh` now runs the Matroska and WebM
cases too (track switching, text subtitles, seeks with and without Cues,
http reconnects) plus damaged files (`mkv-huge-tracks`, `mkv-huge-block`,
`mkv-corrupt`, `mkv-truncated`, from `tool/mkv_cases.py`). Decisions:

- The probe is MP4 first, then Matroska on a second open of the input:
  Matroska rejects anything without the EBML magic at its first read, and
  the error reported is MP4's, so MP4 behaves exactly as before. Over
  http a Matroska file costs one extra request at open.
- Matroska stores only pts. H.264 and HEVC packets get a dts from the
  smallest pts among the next 16 packets of the track (kept monotonic),
  which is all the decoder's reordering rule needs.
- Codec delay (Opus) comes off the block timestamps; AudioToolbox drops
  the decoder's own delay (120 samples for Opus, 529 for MP3) from the
  first output, which moves its pts later by what is missing.
- The unit tests compile the demuxer and the text subtitle decoder into
  `lychee_unittests` when the library lacks them, so FFmpeg builds (CI)
  run them too (not on Windows: `data_source.cc` uses `fseeko`).
- On this M2 Pro VideoToolbox decodes VP9 only in hardware (no software
  VP9 decoder: `--no-hw` leaves the video track off) and has no AV1
  decoder at all (-12906): `av1.mkv` plays its audio with the video
  track off. AV1 on an M3 or A17 Pro, and VP9 on iOS 26.2, are not
  verified. A/V sync of Opus and MP3 against a reference clip is not
  measured; only that the clock and the audio run to the end together.

Fragmented MP4 and tx3g subtitles (2026-10-26): `Mp4Demuxer` reads a moov
with `mvex`/`trex` and its `moof`/`traf`/`tfhd`/`tfdt`/`trun` fragments,
one moof at a time (memory stays one or two fragments), from files and
http, including the moov's own samples before the first moof (FFmpeg's
`frag_keyframe` without `empty_moov`), a moof per track (`separate_moof`)
and Smooth Streaming's layout (no `tfdt`, negative composition offsets,
which shift dts down as FFmpeg's `dts_shift` does). `TextSubtitleDecoder`
turns tx3g samples (UTF-8, or UTF-16 after a BOM) into plain text cues, as the FFmpeg build shows them
(styles dropped). `frag-*`, `http-frag*` and `subtitle-mov-text*` cases
cover them; unit tests cover `mehd` and fragments without `tfdt`, which
FFmpeg's muxer does not write. Decisions:

- Seeks find the fragment in `mfra` (read from the end through `mfro`),
  else in a `sidx` before the first moof that covers the whole file
  (`global_sidx`); else the moofs are read from the last one known, up
  to the target (a range request each on http). In a fragment without a
  keyframe at or before the target, the one before it is tried.
- The duration is `mehd`'s, else where the last fragment ends: from the
  last index entry, or, without an index, by reading every moof at open
  (fast on files; one range request per fragment on http). `mvhd` covers
  only the moov's samples in fragmented files and is not used.
- Not read: a `sidx` of `sidx` boxes and DASH's per-segment `sidx` (the
  moofs are read instead), and fragmented input of unknown size (a live
  fMP4 stream): it plays without a duration or seeks.

Left:

1. Shared IO on Android and Windows: an http `DataSource` there
   (Android's `HttpURLConnection` or Cronet through JNI, WinHTTP), which
   their FFmpeg-free builds (4, 5) need anyway; then FFmpeg reads through
   it as on Apple. One `NSURLSession` per demuxer would let HLS segments
   reuse connections.
2. Containers: MPEG-TS, HLS (fragmented MP4 and tx3g are done, above).
   `Mp4Demuxer` turns `dOps` and `dfLa` into FFmpeg-style extradata
   (OpusHead, STREAMINFO), so MP3, Opus and FLAC in MP4 decode on Android
   and through `ATAudioDecoder`. Bitmap subtitles (PGS, DVB, VobSub) have
   no FFmpeg-free decoder.
3. Packaging: a `tool/build_apple.sh` variant (or the Swift package) that
   ships the FFmpeg-free core; iOS compiles (arm64, zero warnings) but has
   not run, and the plugin and Swift package still link FFmpeg.
4. Windows: decode with Media Foundation (flutter-airplay's
   `windows_video.cpp`: H.264/HEVC MFTs on D3D11) into the same
   `LYCHEE_FRAME_D3D11_TEXTURE` frames, ahead of FFmpeg's D3D11VA, which
   stays the fallback when FFmpeg is present (HEVC without the Store's
   HEVC extension, VP9). Audio through Media Foundation's AAC decoder.
5. Android: done for files (above). Left: http through JNI, an
   FFmpeg-free AAR variant if native Android users want one, and the
   Flutter plugin on it (its example opens http URLs).
6. Malformed input: the MP4 parser bounds sample tables, single samples
   (64 MiB, inside the file), moof boxes (32 MiB, 2 M samples, 64 K
   samples without table bytes, all the scans together 16 M) and its
   allocations (`huge-sample-table`, `huge-sample`, `truncated-file`,
   `corrupt-file`, `frag-truncated`). Fuzzed (2026-10-26):
   `tool/fuzz.sh` runs a libFuzzer target (`core/fuzz`) over
   `Mp4Demuxer` on an in-memory `DataSource`, with tx3g text, under ASan
   and UBSan, seeded with small files in every layout (moov, faststart,
   fragmented with mfra, sidx, moov samples, DASH, a moof per track,
   ismv, negative composition offsets, HEVC/ALAC, mov_text plain and
   fragmented). About an hour of CPU in all (5.6 M inputs, 2 workers)
   found two bugs, both UBSan: a QuickTime sound description's rate as a
   NaN or huge double converted to int, and `Rescale` overflowing on
   tick counts far beyond a small timescale (an mfra time). A minute's
   run after main merged in other work found a third, an OOM: an `stz2`
   with a field size of 0 reads no bytes, so its count alone filled
   memory. All are
   fixed and their inputs are in `core/fuzz/regressions`, which a unit
   test replays (without sanitizers) and `tool/fuzz.sh` replays first.
   Homebrew's LLVM is a development-only dependency (Apple's clang has
   no libFuzzer runtime); CI does not fuzz. Left: fuzz on CI (a Linux
   job can build the parser alone, it uses no Apple API) and keep the
   corpus somewhere. `MatroskaDemuxer` bounds header elements (16 MiB),
   Cues and Attachments (64 MiB), blocks (64 MiB), cue points and
   clusters (4M each) and resyncs at most 16 MiB ahead, but has never
   been fuzzed: a target like the MP4 one, before it ships.

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
4. **CI on Linux**: `core-linux` in `ci.yml` (2026-10-26) runs `NO_HW=1
   tool/regression.sh` on `ubuntu-24.04` on every push, so Linux build
   breaks no longer wait for a WSL run. The core links the LGPL FFmpeg
   from `tool/build_ffmpeg.sh linux-x86_64` (`tool/release.sh build
   linux`; apt's 6.1 predates the API the core uses), cached on the
   script and the source pin like the Apple builds. The media come from
   BtbN's GPL ffmpeg 9.0.2 of the release the plugin pins, the version
   Homebrew's has on macOS. The realtime cases play through a PulseAudio
   started in the job, which with no sound card plays to its `auto_null`
   sink on the system clock. An idle null sink renders 2 s ahead, so a
   first stream waited ~2 s for its audio (`realtime-rate`,
   `realtime-loop` and `http-stall-seek` failed with ~1 s of drift); the
   job keeps a silent 20 ms stream playing so it renders in small
   blocks. Run by hand in WSL (Ubuntu 22.04, gcc 11; a private
   PulseAudio unpacked with `apt-get download`, not WSLg's): the FFmpeg
   build (57 s), the media and all 85 cases pass, the realtime ones and
   `http-stall-seek` 5 times in a row; without the silent stream every
   start showed the ~1 s. gcc 15 on the 26.04 VM builds the core, CLI
   and tests without a warning; the runner's gcc 13 is untried against
   `regression.sh`'s zero-warning check. Waiting on a push to see the
   job on GitHub. TSan stays on macOS: on Linux it needs clang,
   suppressions for FFmpeg, and is far slower than real time. Done when
   the job is green on GitHub.
