# Roadmap

State as of 2026-10-26: the core plays files and network streams through
FFmpeg, with accurate seek, pull-based frame selection, playback rate
(WSOLA), looping and reopening, track selection, text and bitmap
subtitles, metadata and cover art. The Flutter plugin runs on macOS and iOS
(VideoToolbox), Android (MediaCodec), Windows (Media Foundation, then
FFmpeg's D3D11VA) and Linux (VAAPI, never seen working).
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

1. **Realtime timing is checked only on a local machine**: the
   `realtime=1` cases play on the real clock and audio device, so a
   hosted VM's load shows up as drift. CI's macOS jobs failed on them
   three times on 2026-10-26: `realtime-loop` at `max_drift=20.0ms`
   (under 20 allowed), and `realtime-rate` twice with 5 and 7 glitches.
   So CI sets `REALTIME_ADVISORY=1` (2026-10-27): those cases still run
   (the CoreAudio and PulseAudio sinks, slow and stalled servers) with
   their functional checks, but drift, glitches and drops only print and
   annotate a warning; `http-slow`, `http-stall-seek` and the rest of
   `http-hls-live` never flaked and stay strict. Done when a quiet runner
   (self-hosted Mac) runs them without the switch. The virtual-time
   flakes of that day (`live-resolution`, `live-jitter`,
   `subtitle-mid-cue`) are fixed: time ran ahead of a slow live decoder,
   and of events still waiting for the control thread's next 10 ms tick;
   24 runs six at once now give the same counts every run.
   `live-latency` missed its `live_vbuf` range once (2026-10-27), in a
   run of most cases at once on the Mac; five runs since did not repeat
   it. CI's TSan job missed `loop-ts`'s 640x360 event once: virtual time
   ran past the rendition between two control-thread ticks, so it now
   waits for a size change to be published, as for subtitles. Also once
   on CI's TSan job (2026-10-27): `http-lost` failed at the open with
   "HTTP 503" instead of "input ended at": the Apple http source reads up
   to 16 MB ahead, so the server's drop at 3 MB and the refused retry
   can come before a slow open has finished. Not yet made robust.
2. **A video stream switched on over input that cannot seek starts at
   the read position**: on input that cannot seek
   (`lychee_player_is_seekable()` 0, e.g. an http server without range
   requests through FFmpeg), a resync replays each stream's packets
   since the keyframe before the position (`PacketHistory`, 2026-10-27),
   so an audio track switch, a new video device and a decoder rebuild
   resume where playback is (`http-no-ranges-track*`,
   `http-no-ranges-decoder-lost`). Every audio stream is read for that,
   but only the selected video stream: another video track, or video
   turned back on, has no packets before the read position, so its
   first frame comes up to the read-ahead (~4 s) late. Done when that
   case resumes at the position too (reading every video stream costs
   memory per stream), or is judged not worth it.
3. **Styled ASS is slow in the iOS 18.1 simulator**: `sub_ass_styled.mkv`
   in the Flutter example on an iPhone 16 simulator with iOS 18.1 shows
   ~2.6 fps (6 rendered, 4 dropped after 5 s; software decoding, as
   always in the simulator), where `h264_aac.mp4` shows 24 to 30 fps
   with none dropped (2026-10-26). The same with the plugin's pod and
   its Swift package, so not the frame driver; macOS plays the file at
   30 fps. Not investigated: libass with Core Text there (whose Chinese
   fallback that iOS lacks, Features 3), or the software decoder. Done
   when the cause is known and the file plays at its rate in the
   simulator, or the cause is the simulator's alone.

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
   pixel transfer, swscale's default relative colorimetric intent for
   software decoding: 203 nits as white, highlights above it clipped;
   `hdr-tonemap*`). HLG comes out darker through VideoToolbox (98 for a
   0.5 signal) than through swscale (142, what BT.2408 gives); that is
   Apple's mapping. Missing:
   - Flutter: textures are 8-bit SDR on every platform, so the plugin
     stays on tone-mapped BGRA.
   - Windows: the D3D11 video processor gets each frame's DXGI color
     space (`ID3D11VideoContext1`). On an RTX 4070 Ti it maps HLG to SDR
     (124 for a 0.5 signal, against swscale's 142) but clips PQ to white
     even with HDR10 metadata, so since 2026-10-26 PQ leaves it still in
     PQ (R10G10B10A2) and a shader of ours tone-maps it on the GPU like
     swscale (`D3D11VideoProcessor`, constants explained there).
     `lychee_cli --d3d11` readbacks against swscale: 0.5 PQ gray 182
     (181), the bars' center 255 (255), a saturated bright red 255,119,91
     (255,62,29). Grays and in-gamut colors match within 1; saturated
     colors at or above white are an approximation (within 67 over 84
     test colors, median 11), since swscale clips in IPT and the shader in
     ICtCp. Left: HLG through the shader too, so it matches swscale's 142;
     HDR output, which needs an R10G10B10A2 or FP16 texture type.
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
3. **libass on Windows and Linux**: styled ASS goes through libass
   (`LYCHEE_WITH_LIBASS`, pictures like bitmap subtitles) in every macOS,
   iOS and Android build since 2026-10-26: `tool/build_libass.sh` builds
   libass 0.17.5 with FreeType 2.15.0 and HarfBuzz 15.0.0 linked in
   (exporting only libass's API; about 1.4 MB per architecture on Apple,
   1.6 MB per ABI on Android) and FriBidi 1.0.17 as its own library
   (LGPL 2.1), shipped as `libass.xcframework` and
   `libfribidi.xcframework` and inside the AAR. Apple finds fonts through
   Core Text. Android has no font provider: the core embeds Liberation
   Sans Regular and Bold (SIL OFL, metrically compatible with Arial, the
   font most scripts name; 825 KB per ABI in `liblychee.so`; libass's own
   bold is only a slight emboldening) and falls back to
   `/system/fonts/NotoSansCJK-Regular.ttc` (its first face, Japanese
   shapes for Han) for glyphs those lack; other scripts the media carries
   no font for (Arabic, Thai, Indic, emoji) show as boxes there. Seen
   2026-10-26: `sub_ass.mkv` and `sub_ass_styled.mkv` (bold, italic,
   colour, rotation, `\pos`, Hebrew, Chinese) in the Flutter example on
   macOS and the Pixel 10 emulator and in the Swift example on macOS,
   laid out like `lychee_cli --subtitle-ppm` with Homebrew's libass,
   which the shipped macOS libass matches byte for byte, and in the
   Flutter example on an iPhone 16 simulator with iOS 26.5. Core Text's
   fallback for Chinese on iOS is `PingFangUI.ttc`, an Apple Hierarchical
   Variable Font, which FreeType opens only through the system's libhvf
   (`-Dhvf=enabled` on Apple): iOS 26.5 shows the Chinese line, the iOS
   18.1 simulator lacks it and shows boxes (not seen on an iOS 18 device).
   Left: Windows (DirectWrite) and Linux (fontconfig), whose plugins
   compile the core without libass, so they show ASS as plain text; build
   libass there like the Windows and Linux FFmpeg (Platforms 1 and 2).
   Done when the Flutter example on Windows and Linux shows
   `sub_ass_styled.mkv` styled.

## Platforms

Every platform plays through FFmpeg software decoding and CPU RGBA frames
except Apple's (VideoToolbox, `CVPixelBuffer`), Android's (MediaCodec
rendering straight into the `SurfaceProducer`'s Surface), Windows'
(Media Foundation or D3D11VA, shared D3D11 textures) and Linux's (VAAPI
dmabufs, unverified above). What is left per platform is a renderer in
`examples/glfw` (Metal on macOS; Direct3D 11 on Windows since 2026-10-26,
shared textures and CPU frames, checked with h264, hevc10, rotated and vp9
in a window; OpenGL ES on Linux since 2026-10-26, Platforms 2) and its own
`tool/regression.sh` run.

1. **Windows leftovers**: hardware decoding runs through Media Foundation
   (H.264, HEVC; FFmpeg-free builds 4) and otherwise FFmpeg's D3D11VA, on
   Flutter's adapter, and frames reach Flutter's `GpuSurfaceTexture` as
   shared BGRA textures (h264, hevc10 and vp9 with seeks and 1.5x: 0
   dropped; the integration test passes; 2026-10-25, again with Media
   Foundation 2026-10-26). FFmpeg is now our
   own (2026-10-26): `tool/build_ffmpeg.sh windows-x86_64` cross-compiles
   the decode-only build with mingw-w64 (WSL: ~60 s), nothing
   autodetected: w32threads, Schannel, zlib and the D3D11VA hwaccels, with
   libgcc, zlib and the mingw runtime static, so the DLLs import only
   kernel32, msvcrt, bcrypt, crypt32, ncrypt, secur32 and ws2_32 (the
   script checks). 8.4 MB of DLLs (3.5 MB zipped) against BtbN's ~120
   MB, and LGPL 2.1 (no OpenSSL). FFmpeg 9's Schannel code needs the udp
   protocol even without DTLS. `release.yml` builds it on ubuntu-24.04 and
   the release carries `ffmpeg-windows-x86_64.zip`, which
   `flutter/prebuilt.json` pins and `flutter/windows/ffmpeg.cmake`
   downloads (`LYCHEE_PREBUILT_URL` mirrors it; `LYCHEE_FFMPEG_ROOT`,
   CMake or environment, overrides it). 1.0.0-beta.1 carries none, so
   until the next release pins one the plugin falls back to BtbN's pin:
   delete that fallback then. Checked on Windows with the WSL build served
   as the pinned asset: the example plays `h264_aac.mp4`, `hevc10.mp4` and
   `vp9_opus.webm` with D3D11VA (`hw`), seeks and 1.5x, 0 dropped, and an
   https mp4 through Schannel; the integration test passes; a fresh app
   builds from the packed package with the asset from a local mirror, no
   build warnings. The https file drops 1 frame at 1.5x near its end, and
   so does its local copy: the file, not TLS. Not seen: AV1 (WSL's
   mingw-w64 8.0 headers have no `DXVA_PicParams_AV1`, so the WSL build
   has no AV1 hwaccel and FFmpeg no AV1 software decoder; whether the
   runner's 11.0 headers enable it is unknown until `release.yml` runs),
   the `release.yml` job itself, and `lychee_cli` on Windows with this
   FFmpeg.
   Unverified there: the pixel buffer path for a Flutter without a D3D11
   adapter, multiple monitors, audible output, and `tool/regression.sh`
   (needs bash). The decoder is rebuilt at the position when the adapter
   changes (`lychee_player_set_video_device()`) or a hardware decoder fails
   midway, but only a simulated loss has run (`decoder-lost`); not yet seen:
   a real GPU reset (`dxcap -forcetdr`), which also loses the plugin's own
   device, and an adapter change during playback (the plugin sets the
   adapter once). Media Foundation's HEVC decoder comes with the Store's
   HEVC Video Extensions, installed on the test machine; one without them
   should fall back to D3D11VA at open, which has not been seen.
   In `examples/glfw` (2026-10-26) software-decoded frames of the untagged
   test bars looked more saturated than Media Foundation's and D3D11VA's:
   swscale read untagged HD as BT.601. Untagged `h264_aac.mp4` in a
   960x540 window gave 31.4 dB through D3D11 and 24.2 dB (MAE 8.9)
   through software decoding, and `lychee_cli --d3d11 --frame-ppm`
   34.4 dB. swscale now gets the convention's matrix (2026-10-27; 34.1 dB
   on macOS), which has not yet been seen on Windows.
2. **Linux leftovers**: FFmpeg is our own from the next release
   (2026-10-27), as on Windows. `tool/build_ffmpeg.sh linux-x86_64` builds
   the decode-only FFmpeg on Ubuntu 22.04 (WSL: 53 s with the download,
   8.1 MB of libraries against BtbN's 161 MB) with nothing autodetected
   and GnuTLS for https, so it is LGPL 2.1 (it linked OpenSSL 3, LGPL 3,
   while WSL was thought to have no way to install GnuTLS's headers; root
   from the Windows side can). Its libraries need only libc, libm and the
   system's libva, libva-drm, libdrm, GnuTLS and zlib (`libva.so.2`,
   `libva-drm.so.2`, `libdrm.so.2`, `libgnutls.so.30`, `libz.so.1`,
   sonames unchanged since Ubuntu 20.04 and Debian 11), and glibc 2.35 or
   newer; the script checks the list and refuses OpenSSL. `release.yml`
   builds it on ubuntu-22.04 and the release will carry
   `ffmpeg-linux-x86_64.tar.xz` (2.5 MB: `lib/` with its symlinks,
   `include/`), which `tool/release.sh pin` pins in `flutter/prebuilt.json`
   and `flutter/linux/ffmpeg.cmake` downloads into `~/.cache/lychee_player`
   (`LYCHEE_PREBUILT_URL` mirrors it; `LYCHEE_FFMPEG_ROOT`, CMake or
   environment, overrides it), checking each library's license string.
   1.0.0-beta.1 carries none, so until the next release pins one the
   plugin falls back to BtbN's pin, and arm64 always does (no build of
   ours: needs an arm64 runner and a machine to try it): delete the
   x86_64 fallback then. Checked in WSL (2026-10-27): `NO_HW=1
   FFMPEG_ROOT=... tool/regression.sh` passes except `realtime-loop`
   (WSLg's audio clock, below; max_drift 24 ms) and `seek-vp9` (lands at
   7.474 s for 7.5; the same with BtbN's FFmpeg in WSL, and it passes on
   CI's ubuntu-24.04, so WSL's, not this build's); `lychee_cli` plays two
   public https mp4s through GnuTLS, 0 dropped; the Flutter example and
   its integration test (under Xvfb) build with the packed asset served as
   the pinned file through `LYCHEE_PREBUILT_URL`, downloading it into a
   fresh cache and bundling its libraries, and the test passes (10 file
   switches, 4 player threads left). Not seen: the example's window
   playing (the Windows session was locked, hiding WSLg, the VM was off,
   and Flutter under Xvfb draws nothing a screenshot sees), this build on
   a newer distribution (the OpenSSL one ran on the 26.04 VM), and the
   `release.yml` job itself. GitHub will retire the ubuntu-22.04 image;
   the job can then run in a `ubuntu:22.04` container. Unverified there: the Impeller renderer. Flutter
   makes Skia forget its GL state around a texture's populate but does
   nothing for Impeller (`ResolveTextureImpeller`), so populate now saves
   and restores what it changes. Whether that is enough is unknown: on the
   VM, Impeller (OpenGLESSDF on vmwgfx) garbles a plain Flutter app with
   no plugin as well (a Slider half drawn, text missing, colours not
   updating; 2026-10-25), so only the integration test (lifetime) passes
   there. Done when a machine with a real GPU shows the example playing
   under Impeller with its controls drawn.
   `examples/glfw/main_linux.cc` (2026-10-26) is the Flutter-free
   renderer: GLFW with an OpenGL ES 3 context on EGL, taking frames the
   way the plugin does (CPU RGBA uploaded into a texture; dmabufs
   imported a plane at a time and converted by a shader, with the render
   node from EGL's device query). In WSL under Xvfb it plays
   `h264_aac.mp4`, `rotated.mp4` and `vp9_opus.webm` upright with pause
   and seek (0-2 dropped), on WSLg 177 frames in 6 s with 0 dropped,
   and on the VM's GNOME (Wayland, GLFW 3.4) 0-7 dropped per 12 s: the
   loop stalls 25-65 ms now and then there, with any pacing and without
   it. Its swap does not wait for vsync there (100-140 Hz unpaced), so
   the loop sleeps out the refresh itself. The VM's EGL offers dmabuf
   import (on `renderD128`), but without VAAPI no dmabuf frame has been
   drawn. `--snapshot` reads back what it drew: against FFmpeg's frame at
   the pts it prints, a 1:1 h264 snapshot in WSL is 51.5 dB (vp9 35,
   scaled 28-35; on the VM 29-31) and the next frame 18-20 dB, for
   `h264_aac.mp4`, `rotated.mp4` and `vp9_opus.webm`.
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
  carry dead code. Windows is the exception: Media Foundation decodes
  video in both builds, since it needs no FFmpeg DLL to start and is
  what Windows itself plays H.264 and HEVC with.

Windows without FFmpeg plays MP4 files (2026-10-26): `Mp4Demuxer` over a
file `DataSource`, `MFVideoDecoder` (`backends/media_foundation`, ported
from flutter-airplay's `windows_video.cpp`: Windows' H.264 and HEVC MFTs
on a D3D11 device of their own, frames through `D3D11VideoProcessor` as
with D3D11VA) and `MFAudioDecoder` (Windows' AAC decoder, its resampler to
the sink's format). `MFVideoDecoder` also runs first in the FFmpeg build,
ahead of D3D11VA, which takes over for what Media Foundation refuses (VP9,
HEVC without the HEVC Video Extensions, a failure at open or the first
frame). A loss midway rebuilds on Media Foundation; after two, software
decoding takes over (`lychee_cli --lose-decoder-after`). `lychee_cli
--d3d11` on an RTX GPU: h264_aac.mp4 and hevc10.mp4 (P010) decode with
mediafoundation hw=1, vp9_opus.webm with ffmpeg-hwaccel, all 0 dropped
with seeks and at 1.5x; the FFmpeg-free build plays h264_aac.mp4 (360 of
360) and hevc10.mp4; the Flutter example shows the same decoders with 0
dropped, and its integration test passes.

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

- The probe sends input that starts with the EBML magic to Matroska and
  anything it does not recognise to MP4, so MP4 behaves exactly as
  before and unknown input fails with MP4's error (`ProbingDemuxer`,
  under MPEG-TS and HLS below; it first opened MP4, then Matroska).
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

MPEG-TS and HLS without FFmpeg (2026-10-26): `ProbingDemuxer`
(`media/`) reads the input's first KB and hands the same `DataSource`
to `HlsDemuxer` (`backends/hls`, `#EXTM3U`), `TsDemuxer`
(`backends/mp2t`, sync bytes a packet apart), `MatroskaDemuxer` (the
EBML magic) or, for anything else, `Mp4Demuxer`, whose error unknown
input then reports. It replaced the MP4-then-Matroska probe that lived
in `factory.cc`; the http source keeps what it read, so the look costs
no request of its own and Matroska no longer opens the input twice.
Windows and Android keep `Mp4Demuxer` alone. `NO_FFMPEG=1
tool/regression.sh` now runs every case with TS and HLS media too.
Decisions:

- Our own parsers (~2500 lines with HLS) from ISO/IEC 13818-1 and RFC
  8216, not Chromium's `media/formats/mp2t` and `hls`, which are built
  around MSE's append-and-evict model like its MP4 parser.
- One access unit per video PES packet, as muxers write them, passed on
  as Annex B: `VTVideoDecoder` takes parameter sets in band and starts a
  new session on a new resolution. The SPS is parsed only for the size,
  sample aspect ratio and (H.264) colour at open; HEVC's VUI lies behind
  far more syntax, so HEVC in TS is taken as square pixels.
- AAC goes out one ADTS frame per packet, header kept (the AudioToolbox
  decoder makes its configuration from it), timestamped from the PES
  timestamp plus the samples before it; a frame split across PES packets
  keeps the timeline it started on. A packet's duration is the distance
  to the next one of its stream, so each stream holds one packet back.
- Damage: a continuity counter error passes the damaged PES packet on,
  as FFmpeg does (decoders conceal; dropping it lost nearly every 1080p
  frame at one lost TS packet in a hundred); duplicates and packets with
  `transport_error_indicator` are dropped, PSI sections need a valid CRC,
  lost sync is found again from two sync bytes a packet apart. Bounds:
  32 MB per PES packet, 4 KB of section, 4 MB read at open, 16 MB of
  playlist, 200000 segments. M2TS (192-byte packets) is read too.
- Timestamps unwrap past 2^33 next to the previous one; the start is the
  earliest timestamp, the duration from the timestamps in the last 1 MB
  (growing to 16 MB past damage). `resolution_change.ts` lasts 6.029 s
  (its last audio frame's end) where FFmpeg says 6.021 s.
- Seeks bisect on the reference stream's PES timestamps (video if
  enabled), then read forward from 1, 2, 4 ... 64 s before the target,
  then from the start, to the keyframe at or before it; audio starts
  with the frame playing at that keyframe. A window reads only up to
  where the previous one began, so a seek reads the GOP before the
  target about once: `seek-ts` (`offset.ts`, whose first GOP is longer
  than the 7.5 s target) reads ~2.4 MB, 0.18 s in a debug build but
  ~1.5 s under ThreadSanitizer, where the case counts one stall (the
  CLI's limit is 2 s with the decode after it; CI runs TSan only with
  FFmpeg). Each window and bisection step costs a range request over
  http. A source without range requests does not seek, as with FFmpeg.
- HLS master playlists play the highest `BANDWIDTH` whose `CODECS` are
  H.264, HEVC or AAC (or unlisted), the larger picture on a tie; FFmpeg's
  build plays the first variant. No switching. VOD seeks open the
  segment holding the target; the player drops the frames before it.
  Live input starts at the last segment three target durations from the
  end, reloads a target duration after a reload that brought segments
  (half of one otherwise), skips ahead when it fell out of the window,
  and fails after three failed reloads. Each discontinuity sequence gets
  one offset onto a continuous timeline, fixed when it is first entered
  (playing or seeking). Segment URLs resolve against the playlist's URL
  after redirects; reloads ask for the URL first asked for.
  `EXT-X-BYTERANGE` is read.
- Refused with an error: `EXT-X-KEY` (AES-128, SAMPLE-AES) and
  `EXT-X-MAP` (fragmented MP4 segments: `Mp4Demuxer` reads fragments now,
  but HLS does not feed it yet; Left 2). Alternative renditions (`EXT-X-MEDIA`) are not
  played: a variant whose audio is only in a group plays without it.

New cases: `ts-hevc10`, `ts-lost-packets`, `ts-corrupt`, `http-ts`,
`http-hls-seek`, `http-hls-master`, `http-hls-live` (real time; the
media server's `--live` serves a VOD playlist as a growing window),
`seek-ts*`, `loop-ts` and `http-hls-discontinuity*`, all in both builds.
`http-seek-failed` and `http-loop-seek-failed` too: the FFmpeg-free TS
demuxer makes the same three requests before the seek.

The FFmpeg build matches it there since 2026-10-27, in `FFmpegDemuxer`
for formats FFmpeg marks `AVFMT_TS_DISCONT` (MPEG-TS, HLS). A decode
time that goes back is a discontinuity: each stream finds it by itself,
and all take the offset of the first one there, kept across seeks. A
seek reads on from 1 s before the target (backing off to 64 s, then the
start) and hands on the packets from the last keyframe at or before it;
a seek to the start is a byte seek to the first packet. FFmpeg's own TS
seek bisects to a packet, not a keyframe, and its HLS seek drops every
packet past a discontinuity where timestamps start over, so a seek
there reads on from before the first discontinuity (from the start if
none has been read yet: `http-hls-discontinuity-late-seek`). Long
streams make that slow; only an HLS demuxer that knows the segments,
as the FFmpeg-free one does, would do better. The FFmpeg build places
a later segment where the video before it ended, the FFmpeg-free one by
the playlist's times (2 ms apart there). Untested: a single TS file
whose timestamps start over (no test media).

Left:

1. Shared IO on Android and Windows: an http `DataSource` there
   (Android's `HttpURLConnection` or Cronet through JNI, WinHTTP), which
   their FFmpeg-free builds (4, 5) need anyway; then FFmpeg reads through
   it as on Apple. One `NSURLSession` per demuxer would let HLS segments
   reuse connections.
2. Containers: MPEG-TS, HLS, fragmented MP4 and tx3g are done (above).
   HLS with `EXT-X-MAP` (fMP4 segments): a `DataSource` that joins the
   init segment to each media segment, opened by `Mp4Demuxer` (its
   fragments carry `tfdt`), in place of the TS reader. HLS also lacks
   AES-128 (CommonCrypto, CBC per segment with the key fetched once),
   `EXT-X-MEDIA` audio renditions, variant switching, and connection
   reuse: every segment opens its own `NSURLSession`. MP3 in TS goes to
   `ATAudioDecoder` whole, untested; AC-3 has no FFmpeg-free decoder;
   DVB and ID3 timed metadata in TS are ignored. `Mp4Demuxer` turns
   `dOps` and `dfLa` into FFmpeg-style extradata (OpusHead, STREAMINFO),
   so MP3, Opus and FLAC in MP4 decode on Android and through
   `ATAudioDecoder`. Bitmap subtitles (PGS, DVB, VobSub) have no
   FFmpeg-free decoder.
3. Packaging: done for Apple (2026-10-26). `NO_FFMPEG=1
   tool/build_apple.sh` builds `lychee_noffmpeg.xcframework` (inside it
   still `lychee.framework`, so code that opens or imports `lychee` does
   not change; 636 KB for iOS arm64, 1.3 MB for each universal slice,
   only system libraries). A separate xcframework rather than a second
   slice set in `lychee.xcframework`, because an xcframework has one
   library per platform. The Swift packages (`sdk/apple` and the root
   one) pick it with a trait: `FFmpeg` is the default, `NoFFmpeg` takes
   the core alone, e.g. `.package(path: "sdk/apple", traits:
   ["NoFFmpeg"])`; `swift test --traits NoFFmpeg` skips the tests whose
   media (PGS, MP3 in a file the core alone cannot open) needs FFmpeg.
   The Flutter plugin keeps FFmpeg. `tool/release.sh` builds, packs and
   pins the new asset with the others. First run on the iOS simulator
   (iPhone 16, iOS 18.1; the Swift example with the trait):
   `h264_aac.mp4` plays in hardware, 151 frames, 0 dropped in 5 s; HLS
   from `tool/media_server.py` (`/hls/index.m3u8` over http to
   127.0.0.1, which ATS allows) in hardware, 219 frames, 0 dropped in
   7.3 s; `vp9_opus.webm` plays its Opus audio only, the video refused
   ("VP9 needs iOS 26.2": VideoToolbox decodes VP9 from there, and no
   iPhone 16 runtime here is that new). Waiting on the next release: the
   root `Package.swift` still pins 1.0.0-beta.1, which has no
   `lychee_noffmpeg`; once a release carries it, check `swift build
   --traits NoFFmpeg` against the URL package from a scratch app.
4. Windows leftovers: without FFmpeg there is no software video
   decoder, so a machine without a GPU (or a codec its GPU lacks) plays
   no video; Media Foundation's software mode would cover it. AAC in
   ADTS and other audio codecs have no decoder there, and http(s) needs a
   `DataSource` (WinHTTP). Packaging the FFmpeg-free Windows core for the
   plugin is not started.
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
   `MatroskaDemuxer` bounds header elements (16 MiB), Cues and
   Attachments (64 MiB), blocks (64 MiB), cue points and clusters (4M
   each) and resyncs at most 16 MiB ahead. Since 2026-10-26 every parser
   of untrusted input has a target (`core/fuzz/fuzz_targets.h`): `mp4`,
   `matroska` (with its text subtitles), `mpegts` (`TsDemuxer` through
   `TsReader`, `TsParser` and the elementary streams), `hls_playlist`
   (the parser and `PickHlsVariant`) and `hls` (`HlsDemuxer` on one
   input holding the playlist, the variant and the segments, served by
   its `set_opener` hook; playlists are made VOD, so the live reload
   path, which waits real time, is not fuzzed). Seeds are ffmpeg-made
   files per layout: Matroska H.264/AAC, HEVC/FLAC, MP3, VP9/Opus,
   AV1, live WebM, small clusters, SRT/ASS/WebVTT, attachments and
   `tool/mkv_cases.py`'s six damaged layouts; TS with H.264, HEVC, MP3,
   AC-3, E-AC-3, M2TS, a 33-bit wrap, resent headers and a PID switch;
   HLS media, master, byte-range and discontinuity playlists. About ten
   minutes of two workers per target on the Mac (20 CPU-minutes each;
   mp4 2.1 M inputs, matroska 335 K, mpegts 208 K, hls_playlist 12.6 M,
   hls 782 K), another 20 CPU-minutes of matroska after its fixes (324 K
   inputs, nothing found), plus short runs in WSL, found three
   bugs, all UBSan and all fixed with their inputs in
   `core/fuzz/regressions/TARGET`: `ParseHlsPlaylist` overflowed on a
   huge EXTINF or TARGETDURATION (now clamped to 2^24 s); Matroska's
   `ToMicros` overflowed on a cluster time near its 2^52 clamp with a
   large TimestampScale (now clamped to 2^33 s, as MP4's `Rescale`);
   and EBML lacing shifted by -1 after a lace size failed to read.
   `core/fuzz` is its own CMake project that compiles the parsers
   without the library, so it builds on Linux too: CI's `fuzz` job
   (ubuntu-24.04, apt's clang 18) replays the regressions and fuzzes
   each target for a minute on `parsers` changes. On Linux `tool/fuzz.sh`
   runs the fuzzers under `setarch -R`: WSL2's kernel (6.18) has 32 bits
   of mmap randomness, under which clang 14's and 15's sanitizer
   runtimes crash at random. Left: keep the corpus between runs, and
   fuzz the live HLS reloads (a clock the fuzzer can advance). `VTVideoDecoder` gives up after its
   consecutive-error limit on TS that lost one packet in a thousand
   (`ts_lost_packets.ts`), where FFmpeg's software decoder conceals the
   damage and plays on, so `ts-lost-packets` plays it audio only.

The goal is FFmpeg-free builds on Apple platforms, Windows and Android.
Linux keeps FFmpeg: it has no system audio decoder or demuxer.

## Distribution and infrastructure

1. **LGPL FFmpeg**: every plugin build now ships an LGPL-only FFmpeg as
   dynamic libraries: macOS and iOS as xcframeworks from
   `tool/build_ffmpeg.sh` + `tool/package_ffmpeg_apple.sh` (the macOS
   example loads only them, 2026-10-25), Android from `tool/build_ffmpeg.sh`,
   Windows from the release (our build, LGPL 2.1 with Schannel; Platforms
   1), Linux as BtbN's LGPLv3 download (slimming it: Platforms 2). The
   Apple and Android builds enable only the formats the core plays (about
   7 MB per architecture, from 15; 2026-10-25), and
   `FFMPEG_ROOT=... tool/regression.sh` checks them. Android's links
   mbedTLS for https, so it is LGPLv3; the core gives mbedTLS Android's
   CA store as a file in the app's cache directory. Releases build
   `macos-x86_64` with assembly (`tool/release.sh build` requires `nasm`;
   the build Mac's local one has none). AV1 decodes only through VideoToolbox (M3 and later):
   add dav1d (BSD) if software AV1 matters. Android before 11 finds the
   cache directory from the package name (no `TMPDIR`), never run there.
   Apps must ship the FFmpeg license notice and say where its source is;
   releases attach the FFmpeg, Mbed TLS and libass sources they were
   built from, and `THIRD_PARTY_NOTICES` (Chromium, FFmpeg, Mbed TLS,
   libass and what it links, Liberation Sans, with the license texts)
   goes into `lychee.framework`, the AAR (as the Java resource
   `META-INF/lychee/THIRD_PARTY_NOTICES`, so it lands in APKs) and the
   pub package (`core/`). Only the
   development CMake build (CLI, GLFW example, `tool/regression.sh`
   without `FFMPEG_ROOT`) still links Homebrew's GPL FFmpeg.
   BtbN's Linux build (and the Windows fallback) links OpenSSL, dav1d,
   libvpx, libaom, Opus, LAME, libass and more statically (their avcodec
   configuration),
   whose notices we do not ship; `THIRD_PARTY_NOTICES` now says so and
   names BtbN's revision (n9.0.2-17-g2a571b6068), which is not the
   ffmpeg-9.0.2 source the releases attach. Our own FFmpeg-only builds
   fix both: done for Windows (its notices add zlib and the mingw-w64
   runtime), left for Linux (Platforms 2).
   The playback matrix runs through the shipped macOS FFmpeg
   (`FFMPEG_ROOT=third_party/ffmpeg/macos-arm64`) in CI's `apple` job
   and in `release.yml` before anything is packed (2026-10-26; its
   first GitHub run failed only on the live flakes fixed since). CI also
   runs `swift test` and the Rust crates now (item 5); the Android and
   Flutter lifetime tests run only by hand.
   `tool/release.sh build` reuses any `third_party/ffmpeg/PLATFORM` (and
   `third_party/libass/PLATFORM`) that exists, so a changed
   `tool/build_ffmpeg.sh` (or `tool/build_libass.sh`) needs that directory
   deleted by hand (CI keys its caches on the scripts, so it rebuilds);
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
   the root `Package.swift` ignores it, and the plugin's Swift package
   takes only an https one (SwiftPM downloads nothing else). Windows and Linux compile the core from a copy
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
   The next release adds `libass.xcframework.zip` and
   `libfribidi.xcframework.zip` (eight frameworks) and the libass,
   FreeType, HarfBuzz, FriBidi and Liberation fonts sources; the pod and
   the root `Package.swift` take a release's frameworks from its pins, so
   the beta.1 pins keep working. Checked here (2026-10-26): every release
   step into a scratch directory, ending with `tool/release.sh check`
   (fresh macOS, iOS simulator and Android apps download all eight and
   carry libass); not yet run on GitHub's runners, which now install
   meson.
   Later: the AAR on Maven Central too (what native Android users
   expect; needs a Sonatype account for `tech.soit`); the next release
   carries the Linux FFmpeg (`ffmpeg-linux-x86_64.tar.xz`, LGPL 2.1 with
   GnuTLS; Platforms 2) in place of BtbN's, after which the x86_64 BtbN
   fallback in `flutter/linux/ffmpeg.cmake` goes (the job, the pack step
   and the CMake are done, 2026-10-27; not yet run on GitHub);
   `tool/release.sh check` building
   a Windows app (it runs on macOS; the Windows check above was by hand);
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
      The Flutter plugin builds as a Swift package too (2026-10-26,
      `flutter/darwin/lychee_player/Package.swift`; was Platforms 4),
      for Flutter's Swift Package Manager integration, and there pulls
      frames through the SDK's `LycheeFrameDriver` (now an `NSObject`
      with `@objc` API and `init(handle:onNewFrame:)` for players Dart
      owns; `copyFrame` returns +1 to Objective-C, the copy rule). The
      pod keeps `LycheeTexture`, a port of it: a pod cannot depend on a
      Swift package. Decisions:
      - No URL dependency on the root package. It clones the whole
        repository for one product, resolves only once the plugin's
        version is released, and a checkout between releases would
        compile the plugin against the last release's SDK. (The identity
        clash feared here before does not happen: Flutter names the
        symlink after the pub path's directory, `flutter` in this
        repository and `lychee_player-VERSION` from pub.dev.) Instead
        `tool/release.sh` copies `sdk/apple/Sources/LycheePlayer` into
        the package while pub packs, as it does `core/`, and in this
        repository the manifest depends on `sdk/apple` by an absolute
        path from `#filePath` (relative paths resolve against Flutter's
        symlink, and SwiftPM refuses targets and binaries outside the
        package).
      - Frameworks: complete local ones through `sdk/apple`; else the
        release's pinned zips, or the same unzipped into the package's
        `Frameworks/` (`tool/release.sh check`, since SwiftPM downloads
        only over https). A checkout without local builds compiles the
        plugin's copy of the driver against the pinned core, which may
        be older than `sdk/apple` (1.0.0-beta.1 lacks
        `lychee_player_is_seekable`); it builds, and then fails in Dart
        at the first newer symbol, as with the pod.
      - Dart opens `lychee.framework/lychee`: with SwiftPM the plugin is
        a static library in the app, with no framework of its own.
      Checked (2026-10-26): the example with SwiftPM and with CocoaPods
      on macOS (hardware, 30 fps, 0 dropped) and an iPhone 16 simulator
      (software, the same frame counts both ways), `sub_ass_styled.mkv`
      styled in all four; the macOS integration test both ways; a fresh
      checkout's SwiftPM build downloading 1.0.0-beta.1 from GitHub;
      `tool/release.sh check` with both. Left: drop the pod and the copy
      once Flutter drops CocoaPods (it warns that CocoaPods-only plugins
      will become an error; this one has both). Waiting on the next
      release: SwiftPM downloading the eight zips from GitHub for a
      fresh app on `lychee_player` from pub.dev (`flutter create`, add
      the version, `flutter build macos` and `ios --simulator` with
      Swift Package Manager on; `nm` on the app finds
      `LycheeFrameDriver`).
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
5. **Tests by layer, CI by area** (decided by the user 2026-10-26): a
   change is tested at the lowest layer that sees it (unit tests, the
   CLI matrix, the platform's native harness, the bindings' own tests)
   and a Flutter app only for changes to the plugin; AGENTS.md
   ("Verifying a change") has the layers and what each area needs.
   `tool/changes.sh` names a branch's areas from the files it touches,
   and `ci.yml` gates its jobs on them through a `changes` job: core
   changes run `core-macos` (plain, TSan, ASan) and `core-linux`; the `apple`
   job (xcframeworks, the shipped-FFmpeg matrix, `swift test`) runs for
   core, Apple and plugin changes, with Flutter's analyze and example
   build only for `flutter/`; a new `rust` job runs `cargo test`,
   clippy and docs for `rust/` and `lychee.h`; documentation alone runs
   nothing, and a change to `ci.yml` or the script runs everything.
   `MEDIA_ONLY=1 tool/regression.sh` generates the media without a build,
   for the Swift and Rust tests. Checked locally on past commits (e.g.
   the Linux plugin fix gives `linux flutter`, the TS/HLS demuxers `core
   parsers` plus every platform through `factory.cc`); the gated jobs,
   `swift test` and the Rust job on hosted runners wait on a push. The
   native harnesses for Windows and Linux are Platforms' `examples/glfw`
   renderers. Left: CI jobs for Android (the SDK build and its unit
   tests on an emulator) and Windows (the core and `examples/glfw` build
   on `windows-latest`); the harness screenshots stay local.
   Frames are checked by pixels since 2026-10-26: `lychee_cli
   --frame-ppm` and `examples/glfw --snapshot` (macOS and Windows) write
   what is on screen, and `tool/frame_check.py` compares it with
   FFmpeg's decode of the same pts (README, "Checking frames by
   pixels"); the `frame-*` regression cases cover each frame path,
   10-bit, rotation and PQ tone mapping, and a self-test case keeps the
   thresholds telling wrong frames apart. Left: the Linux harness (on
   its own branch), the Swift and Android examples, and
   VideoToolbox's tone mapping of bright HDR colors, which differs from
   swscale's reference (`hdr_pq_bars.mp4`: 27.9 dB).
6. **Compiler and sanitizer checks** (2026-10-26). The top-level
   `CMakeLists.txt` builds the project's own code with `-Wall -Wextra
   -Wimplicit-fallthrough -Wnon-virtual-dtor` (not
   `-Wmissing-field-initializers`: `struct_size`-first C structs and
   aggregates rely on defaulted fields, as in Chromium), so
   `tool/regression.sh`'s zero-warning check holds; the plugins and the
   Android SDK build the core with their own flags. Clean with Apple
   clang (with and without FFmpeg), and in WSL with gcc 11 and clang 14,
   where clang 14 with libstdc++ also found a missing `<cstddef>`.
   `ASAN=1 tool/regression.sh` builds with AddressSanitizer and
   UndefinedBehaviorSanitizer (C, C++ and Objective-C++) and fails a
   case on any report; all 117 cases and the unit tests pass under it on
   macOS, and `core-macos` runs it on every core change. `COVERAGE=1`
   merges clang's coverage of the unit tests and every case: 84.9% of
   `core/src`'s lines, 91.4% of functions, 69.1% of branches on macOS
   with FFmpeg (2026-10-26). `.clang-format` (Chromium; 4% of today's
   lines differ, so it formats changed lines only, `tool/format.sh`) and
   a `format` job, pinned to clang-format 23 from pip, check every push.
   `core/test/c_header_test.c` keeps `lychee.h` C99 under `-Wpedantic`.
   `.editorconfig` has each language's indentation. Waiting on a push to
   see the new jobs on GitHub; the runner's gcc 13 against the stricter
   warnings is untried.
7. **Engineering practices still to add**, in rough order of value
   (the ABI check is done, 2026-10-27: `tool/abi_check.py` and CI's
   `abi` job, AGENTS.md's `abi` area; it reads the header with clang's
   JSON AST and a compiled `sizeof`/`offsetof` probe on the host's ABI
   only, so a layout that differs only on 32-bit or Windows goes
   unseen; its first run on GitHub waits on a push. The stored fuzzing
   corpus is done too, also unseen on GitHub: CI's `fuzz` job restores the newest
   `build-fuzz/corpus` from an Actions cache and saves its own after
   every run, failed ones too; nothing minimizes it yet, so if it grows
   past a few hundred MB, merge it with libFuzzer's `-merge=1`; OSS-Fuzz
   once the project is public):
   - **clang-tidy** on changed files (`bugprone-*`, `performance-*`,
     `modernize-use-override`, `cppcoreguidelines-pro-type-member-init`),
     with a checked-in `.clang-tidy`, local first, then in CI.
   - **MSVC `/W4`** for the Windows build, and warnings as errors in the
     plugins' and the Android SDK's own core builds.
   - **Performance benchmarks**: decode and frame-conversion throughput,
     seek latency and memory per resolution on the virtual clock, kept
     per commit so a regression shows as a number rather than a feeling.
   - **Soak tests**: hours of looping, seeking and file switching on each
     platform's harness, checking memory and thread counts stay flat.
   - **Supply chain**: Dependabot (or Renovate) for the pinned Actions,
     GLFW, FFmpeg and libass sources; SHA-pinned Actions; build
     provenance attestations and an SBOM for release assets
     (`actions/attest-build-provenance`).
   - **CI jobs for Android and Windows** (item 5's left-overs), and a
     coverage report as a CI artifact once coverage is watched.
