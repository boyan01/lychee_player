## Unreleased

### Fixed

* Windows: H.264 and HEVC MP4s whose keyframe index leaves out the first
  keyframes no longer show a still picture until the first keyframe it
  lists, which could be 30 s in.

## 1.0.0-beta.6

### Breaking

* `LycheeVideo`, and `LycheePlayerView` on Apple and Android, now draw
  text subtitles too. If your app draws `subtitle` itself, pass
  `subtitleStyle: null` (Apple: `subtitleStyle = nil`).
* Windows: `LYCHEE_FRAME_D3D11_TEXTURE` textures are reused for later
  frames, from 100 ms after the frame holding one is released. Keep the
  `lychee_frame` until you have drawn from it, not only a texture opened
  from its shared handle; the Flutter plugin already does.

### New

* Add subtitle files and URLs (SubRip, WebVTT, ASS/SSA) to what plays with
  `addSubtitle()` (`lychee_player_add_subtitle()` in C): the file becomes
  a subtitle track, selected once loaded, its times counted from the
  start of the media. Tracks say whether they came from one (`external`).
  Files in GBK, Big5 or windows-1252 are recognized; pass `encoding`
  (such as `'Shift_JIS'`) for others.
* Shift all subtitles later or earlier with `setSubtitleDelay()`
  (`lychee_player_set_subtitle_delay()` in C).
* Style the text subtitles the views draw with `subtitleStyle`: font,
  size, color, outline, background and margin.
* Apple: `LycheePlayer.subtitlePictures(width:height:unlessId:)` copies
  nothing while the pictures on screen are still `unlessId`, so it can be
  called every frame; `LycheePlayerView` no longer copies picture
  subtitles every frame.
* PGS (Blu-ray) bitmap subtitles in Matroska and M2TS show without
  FFmpeg, the default on Android, iOS, macOS and Windows; before, only
  builds with `LYCHEE_WITH_FFMPEG=ON` showed them. DVB and VobSub still
  need FFmpeg.

### Fixed

* Damaged or crafted files no longer crash the player, run it out of
  memory or hang it: Matroska (on 32-bit Android, `armeabi-v7a`), MP4,
  MPEG-TS, HLS playlists, SubRip and picture subtitles.
* Media can no longer make the player open what its source may not
  reach: an HLS stream from the network refuses local files as segments,
  keys or playlists, and on Windows no HLS playlist or IMSC subtitle
  opens a share (`//host/share`) on another machine, which would send it
  the user's Windows login.
* Live audio whose timestamps jump hours ahead no longer hangs the
  player; it waits in silence while the video plays on.
* Live audio no longer breaks up after a pause in the sender's audio or a
  change in its delay when Android, Apple or Windows decoders decode it.
* Video no longer stutters while the audio device starts or stops:
  pausing, resuming, seeking and switching tracks no longer hold up
  frames or other player calls.
* Software-decoded video handed out as CPU frames (Linux, Windows with
  CPU frames, `hw: false`) converts several times faster: 4K HDR no
  longer plays at a few frames per second.
* Builds without FFmpeg (Android's base AAR): fragmented MP4 without an
  index opens without reading every fragment, MP4 and Matroska without
  cues seek in a few reads, and poorly interleaved MP4 plays over HTTP
  without a new request for each switch between audio and video.
* HLS: fragmented MP4 segments play as they download, byte-range
  playlists no longer make a request per segment, and the next segment
  downloads while the current one plays.
* `LycheeVideo` no longer rebuilds five times a second while playing:
  `LycheePlayer` notifies only when its value changes, and
  `LycheePlayerValue` now compares by value.
* Windows: software-decoded video in Flutter no longer holds up drawing
  while its frames upload.
* Windows: HLS reuses its HTTP connection for playlists and segments
  instead of a new TLS handshake for each.
* Seeking in MPEG-TS, Matroska and MP4 over HTTP downloads much less: on
  Apple about a third as much for a large TS file, and it finishes
  sooner; on Windows about half; on Android it no longer downloads far
  past what it reads.
* Live HLS with subtitles no longer uses more memory the longer it plays:
  subtitles that ended over a minute ago are let go.
* MP3 audio no longer starts up to 70 ms late after a seek, nor Opus on
  Android up to 80 ms.
* Android: MP3 audio no longer plays 11 ms ahead of the video.

## 1.0.0-beta.5

### New

* Live input reports how it plays: `lychee_stats` adds dropped video
  packets, late audio, audio silence, how far audio runs behind and the
  audio decoder, and the player logs them every 5 seconds.
* `lychee_codec_supported()` tells, before a stream opens, whether a
  codec can be decoded, in hardware or in software.
* Show players your native code creates in a Flutter `Texture`:
  `LycheeHostTexture` on iOS and macOS, `lychee_host_texture_*` on Linux
  and `LycheeHostTexture*` on Windows. Native code links the core as
  `lychee::lychee` in CMake (Linux, Windows) or through prefab (Android).
* `getStats()` (and `lychee_stats`) also report frames decoded, bytes read
  per track, the download rate, the startup time, rebuffering (count and
  time), seeks (count and the last one's time), how far video runs
  ahead of or behind the audio, the audio device's format and latency,
  the HLS variant playing with its bandwidth, and the audio decoder. The
  player logs when it rebuffers, how long seeks and the
  startup took, and a summary when it closes the media.

### Fixed

* Frame counts in `getStats()` no longer start over after a track change
  or a decoder recovery.

## 1.0.0-beta.4

### Breaking

* iOS, macOS and Windows: FFmpeg is no longer included by default, as on
  Android. MP4, Matroska/WebM, MPEG-TS and HLS still play; other formats
  need `LYCHEE_WITH_FFMPEG=ON`. Run `flutter clean` after switching.
* iOS and macOS: Swift Package Manager builds need Swift 6.1.

### New

* Keep the FFmpeg choice in the app: `LYCHEE_WITH_FFMPEG` in
  `gradle.properties`, `windows/CMakeLists.txt` or the `Podfile`, or
  `LYCHEE_PLAYER_FFMPEG` in an `.xcconfig` for Swift Package Manager.

## 1.0.0-beta.3

* Android now uses an FFmpeg-free SDK by default. Native Android apps can
  add the optional `lychee-ffmpeg` package and call `LycheeFFmpeg.load()`
  before creating a player. Both AARs are published under
  `io.github.boyan01.lychee` on Maven Central, with sources and API docs.
* Android adds armeabi-v7a support alongside arm64-v8a and x86_64.
* HLS gains fragmented MP4, AES-128 encryption, adaptive video variants,
  alternate audio and subtitle renditions through the built-in demuxers.
  Android uses system HTTPS and AES support, including FFmpeg fallback
  streams and redirects from HTTP to HTTPS.
* Android decodes VP8, VP9 and AV1 through MediaCodec. Windows can decode
  VP9 and AV1 through the installed system codec extensions, and supports
  more audio formats without FFmpeg. The optional FFmpeg builds include
  dav1d for software AV1 decoding.
* Add fragmented MP4 WebVTT and IMSC text and image subtitles in HLS.
  Subtitle rendering is now text and bitmap only: libass is removed,
  and ASS subtitles display as plain text without styling.
* Fade audio around seeks, pauses, playback-rate changes and track
  switches, close gaps at loop boundaries, and retry an audio device
  that failed to start.
* Recover iOS hardware decoding after returning to the foreground,
  improve Apple decoding throughput and software H.264 startup, and
  correct HDR tone mapping on Linux and FFmpeg software frame paths.
* Fix HLS byte-range handling, early subtitle cues, transport-stream
  timestamp discontinuities and an FFmpeg HLS subtitle buffer leak.

## 1.0.0-beta.2

* iOS and macOS: the plugin builds with Swift Package Manager as well as
  CocoaPods.
* Styled ASS subtitles through libass on macOS, iOS and Android.
* Windows decodes H.264 and HEVC with Media Foundation first, FFmpeg's
  D3D11VA second, and ships a slimmer LGPL FFmpeg of our own; HDR is
  tone-mapped on the GPU.
* Linux ships our own LGPL 2.1 FFmpeg (GnuTLS, VAAPI) for x86_64 and
  arm64 in place of BtbN's. Hardware decoding through VAAPI is seen
  working on an Intel GPU.
* A hardware decoder that hangs is given up after a few seconds and
  playback goes on in software; disposing a player never blocks on it.
* An audio device that never starts or stops pulling no longer stalls
  playback: it goes on silently until the device comes back.
* Over an http server without range requests, switching the video or
  audio track resumes at the position instead of failing.
  `LycheePlayerValue.seekable` says whether seeks can move playback.
* Untagged HD video is read as BT.709; HLS discontinuities play through
  and MPEG-TS seeks land on the keyframe before.
* A hot restart clears the player's event callback, so no event reaches
  a dead isolate.
* Android is 64-bit only (arm64-v8a, x86_64).
* Fixes from fuzzing the MP4, Matroska, MPEG-TS and HLS parsers.

## 1.0.0-beta.1

* A rewrite: a video and audio player over a C/C++ core, with hardware
  decoding on macOS, iOS, Android, Windows and Linux. The API is new and
  not compatible with 0.x, which played audio only.
* Accurate seeking, playback rate with pitch kept, looping, track
  selection, text and bitmap subtitles, metadata and cover art.
* HDR video is tone-mapped to SDR.
* Android, iOS and macOS take the prebuilt core from the GitHub release,
  checked against pinned SHA-256s; `LYCHEE_PREBUILT_URL` names a mirror.
* FFmpeg is linked under the LGPL: apps must ship the notices in
  `THIRD_PARTY_NOTICES`, which the prebuilt core carries.
