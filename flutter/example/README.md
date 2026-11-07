# Lychee Player example

A small player for local video, audio and network streams. The example uses
this checkout's Flutter plugin.

- Open a file with the folder button, or paste a file path or URL and press
  Enter or **Open**.
- On macOS, Windows and Linux, drop a file anywhere in the window to play it.
  A drop replaces the current media. When several files are dropped, the
  first file opens. Folder and text drops are not accepted.
- On desktop, press Space to play or pause and Left/Right to seek five
  seconds. Shortcuts do not interrupt typing in the path field.
- Seek with the timeline or the ten-second buttons. Change speed, toggle
  looping, and select audio or subtitle tracks from the controls.
- Add an external subtitle from **Tracks → Add subtitle file…**, even when
  the media has no embedded subtitles.
- Open **Playback details** for the decoder, frame rate and dropped frames.
  Playback errors are always visible without opening this panel.

The layout adapts to narrow windows. The example requires Dart 3.13.5 and
Flutter 3.47.7 for `desktop_drop` 0.9.2. The plugin's own minimum is unchanged.

`file_selector` and its platform implementations are pinned together to
commit `79f764c366bdbba0236958e3265db3967f33f8f1` of the upstream
[cross_file 0.4 migration](https://github.com/flutter/packages/pull/13147),
which is not yet published. Switch these Git dependencies back to pub.dev
once that migration is released. No `cross_file` version override is used.

Local files play directly. Provider-backed selections and promised drops
are streamed into temporary files before their provider access expires.
Those copies remain available for seeks and loops until the player page
is disposed. A large provider-backed file therefore takes time and disk
space to import before playback starts.

## Run

See [Development](../../docs/development.md) for the native libraries and
platform setup. From this directory:

```sh
flutter run -d macos
flutter build macos --profile
build/macos/Build/Products/Profile/lychee_player_example.app/Contents/MacOS/lychee_player_example --seconds 4 /tmp/lychee-media/h264_aac.mp4
```

Desktop arguments: `[--seconds N] [--subtitle PATH_OR_URL] [FILE_OR_URL]`.
For mobile runs, use `--dart-define=LYCHEE_URL=...` and optionally
`--dart-define=LYCHEE_SECONDS=4`. Timed runs print a `summary:` line and exit.

## Checks

```sh
flutter analyze
flutter test test/player_page_test.dart
flutter test integration_test -d macos
```

The widget tests cover compact layouts and drop feedback. The integration
suite checks playback after a simulated drop, file switching and teardown;
it does not synthesize an operating-system drag gesture.
