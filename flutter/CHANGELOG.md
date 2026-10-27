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
