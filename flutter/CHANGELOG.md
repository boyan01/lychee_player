## Unreleased

* The consuming app can persist the FFmpeg choice in Android
  `gradle.properties`, Windows `CMakeLists.txt`, a CocoaPods `Podfile`, or
  an Apple `.xcconfig` with the SwiftPM embed integration shown in the example.
  `LYCHEE_WITH_FFMPEG` in the build environment overrides the app default.

* Flutter uses the FFmpeg-free core by default on iOS, macOS and Windows,
  as on Android. Set `LYCHEE_WITH_FFMPEG=ON` in the build environment to
  include FFmpeg using normal Flutter run/build commands. CocoaPods and
  SwiftPM remain supported; the latter requires Swift 6.1. Run
  `flutter clean` when switching backends. Linux keeps FFmpeg.

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
