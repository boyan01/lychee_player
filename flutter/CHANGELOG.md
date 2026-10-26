## 1.0.0

* A rewrite: a video and audio player over a C/C++ core, with hardware
  decoding on macOS, iOS, Android, Windows and Linux. The API is new and
  not compatible with 0.x, which played audio only.
* Accurate seeking, playback rate with pitch kept, looping, track
  selection, text and bitmap subtitles, metadata and cover art.
* HDR video is tone-mapped to SDR.
* Android, iOS and macOS take the prebuilt core from the GitHub release,
  checked against pinned SHA-256s; `LYCHEE_PREBUILT_URL` names a mirror.
