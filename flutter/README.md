# lychee_player

[![pub](https://img.shields.io/pub/v/lychee_player?include_prereleases)](https://pub.dev/packages/lychee_player) [![pub points](https://img.shields.io/pub/points/lychee_player)](https://pub.dev/packages/lychee_player/score) [![CI](https://github.com/boyan01/lychee_player/actions/workflows/ci.yml/badge.svg)](https://github.com/boyan01/lychee_player/actions/workflows/ci.yml) [![License: MIT](https://img.shields.io/github/license/boyan01/lychee_player)](https://github.com/boyan01/lychee_player/blob/main/LICENSE)

A video and audio player for Flutter on Android, iOS, macOS, Windows and
Linux. It decodes on the GPU with each platform's own decoders and shows
frames as Flutter textures, without a copy where the platform allows.

- Hardware decoding: MediaCodec, VideoToolbox, Media Foundation and
  D3D11VA, VAAPI; software when the hardware gives up.
- MP4, Matroska/WebM, MPEG-TS and HLS (adaptive, AES-128, alternate audio
  and subtitles); more formats with the optional FFmpeg backend.
- Accurate seeking, rate 0.25x to 4x with pitch kept, gapless looping,
  track selection.
- Text and bitmap subtitles, metadata and cover art, HDR tone-mapped to
  SDR.

## Install

```sh
flutter pub add lychee_player
```

Then set the minimums under [Platform setup](#platform-setup).

## Usage

```dart
import 'package:flutter/widgets.dart';
import 'package:lychee_player/lychee_player.dart';

class VideoPage extends StatefulWidget {
  const VideoPage({super.key});

  @override
  State<VideoPage> createState() => _VideoPageState();
}

class _VideoPageState extends State<VideoPage> {
  LycheePlayer? _player;

  @override
  void initState() {
    super.initState();
    LycheePlayer.create().then((player) {
      if (!mounted) {
        player.dispose();
        return;
      }
      setState(() => _player = player);
      player.open('https://example.com/video.mp4');
      player.play();
    });
  }

  @override
  void dispose() {
    _player?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final player = _player;
    return player == null ? const SizedBox() : LycheeVideo(player);
  }
}
```

`LycheePlayer` is a `ValueNotifier<LycheePlayerValue>`: listen to it, or
use a `ValueListenableBuilder`, for the state, `position`, `duration`,
`buffered`, `tracks` and the rest. `LycheeVideo` keeps the video's
aspect ratio and rotation (`fit` changes the scaling).

| Call | Does |
| --- | --- |
| `LycheePlayer.create(hardwareDecoding: true)` | A new player. |
| `open(url)` | Opens a file path or http(s) URL; replaces the current media. |
| `play()`, `pause()`, `seek(Duration)` | Seeks are ignored while `value.seekable` is false. |
| `setRate(1.5)`, `setVolume(0.5)`, `setLooping(true)` | Rate is 0.25 to 4, pitch kept. |
| `selectTrack(type, index)` | An index into `value.tracks`; `null` turns that type off. |
| `getMetadata('title')` | A container tag; `value.artwork` has the cover. |
| `getStats()` | Decoders, hardware or not, decoded, rendered and dropped frames, bytes read, download rate, startup, rebuffering and seek times. |
| `dispose()` | Required: releases the native player. |

Subtitles: text arrives in `value.subtitle` for your app to draw over the
video. Bitmap subtitles (PGS, DVB, VobSub) arrive in
`value.subtitlePictures`, and `LycheeVideo` draws them itself.

### Players your native code creates

Native code can create players through the C ABI (`lychee/lychee.h`),
for example to feed a live stream with `lychee_player_open_live()`, and
show them in a Flutter `Texture`. Create the player with the texture's
frame types, set it on the texture, and set none before destroying it;
the headers say which thread each call needs.

| Platform | Texture | Link the core |
| --- | --- | --- |
| iOS, macOS | `LycheeHostTexture` (`import lychee_player`) | Through the plugin |
| Linux | `lychee_host_texture_*` (`lychee_player/lychee_host_texture.h`) | `lychee::lychee` in CMake |
| Windows | `LycheeHostTexture*` (`lychee_player/lychee_host_texture.h`) | `lychee::lychee` in CMake |
| Android | Render to a `Surface` with `lychee_player_set_video_surface()` | `lychee` through prefab |

## Platform setup

| Platform | Minimum | Set in your app |
| --- | --- | --- |
| Android | API 26 | `minSdk = 26` in `android/app/build.gradle.kts` |
| iOS | 16.0 | `platform :ios, '16.0'` in `ios/Podfile` and the Runner's deployment target |
| macOS | 11.0 | `platform :osx, '11.0'` in `macos/Podfile` and the Runner's deployment target |
| Windows | 10 | Visual Studio 2022 |
| Linux | Ubuntu 22.04 | See below |

**macOS**: Flutter's template runs the app in the App Sandbox. Add
`com.apple.security.network.client` to both entitlements files to play
URLs. Local files must be ones the user picked (`file_selector`) or in
the app's container; other paths fail with "Operation not permitted".

**Linux**: building needs `libepoxy-dev`, `libpulse-dev` and
`libpng-dev`. At run time the app needs `libgtk-3-0`, `libpulse0`,
`libpng16-16`, and for FFmpeg `libva2`, `libva-drm2`, `libdrm2`,
`libgnutls30` and `zlib1g`. With NVIDIA's driver, an app that quits while
a video plays can crash in `libnvidia-eglcore`; destroy the window in the
runner's `shutdown`, as the
[example's runner](https://github.com/boyan01/lychee_player/blob/main/flutter/example/linux/runner/my_application.cc)
does.

## FFmpeg

FFmpeg is off by default on Android, iOS, macOS and Windows: the app is
smaller and the system's decoders play MP4, Matroska/WebM, MPEG-TS and
HLS. Turn it on for other formats (FLV, Ogg, ...) or codecs the system
lacks. Linux always uses FFmpeg.

For one build, set the environment variable:

```sh
LYCHEE_WITH_FFMPEG=ON flutter build apk
```

To keep the choice in the app, set it once per platform (the environment
variable still wins):

| Platform | File | Setting |
| --- | --- | --- |
| Android | `android/gradle.properties` | `LYCHEE_WITH_FFMPEG=ON` |
| Windows | `windows/CMakeLists.txt`, before `include(flutter/generated_plugins.cmake)` | `set(LYCHEE_WITH_FFMPEG ON)` |
| iOS, macOS with CocoaPods | `ios/Podfile`, `macos/Podfile`, before `flutter_install_all_*_pods` | `ENV['LYCHEE_WITH_FFMPEG'] \|\|= 'ON'` |
| iOS, macOS with Swift Package Manager | `Flutter/LycheePlayer.xcconfig`, one-time setup below | `LYCHEE_PLAYER_FFMPEG = ON` |

Run `flutter clean` after switching.

<details>
<summary>One-time setup for Swift Package Manager</summary>

Swift Package Manager resolves the plugin before the app's settings are
known, so a build phase picks the core afterwards:

1. Copy
   [`tool/lychee_apple.sh`](https://github.com/boyan01/lychee_player/blob/main/flutter/example/tool/lychee_apple.sh)
   from the example into your app's `tool/`.
2. Create `ios/Flutter/LycheePlayer.xcconfig` and
   `macos/Flutter/LycheePlayer.xcconfig` with
   `LYCHEE_PLAYER_FFMPEG = ON`, and add `#include "LycheePlayer.xcconfig"`
   to iOS `Flutter/Debug.xcconfig` and `Release.xcconfig`, and macOS
   `Flutter/Flutter-Debug.xcconfig` and `Flutter-Release.xcconfig`.
3. In Xcode, append this line to the Runner target's final Flutter embed
   phase (iOS: **Thin Binary**; macOS: the phase running
   `macos_assemble.sh embed`):

   ```sh
   /bin/bash "$FLUTTER_APPLICATION_PATH/tool/lychee_apple.sh"
   ```

   and add this to the phase's **Output Files**, so incremental builds
   sign the app again:

   ```text
   $(TARGET_BUILD_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/lychee_player_backend
   ```

4. Keep **User Script Sandboxing** off for Runner, as Flutter's template
   has it.

Swift Package Manager builds need Swift 6.1. An environment variable set
in an Xcode scheme reaches only the launched app, not the build.

</details>

## Downloads during the build

Android, iOS and macOS download the prebuilt core, and Linux (and
Windows with FFmpeg on) download FFmpeg, from this version's GitHub
release, checked against SHA-256s pinned in the package. Where GitHub is slow or blocked, point `LYCHEE_PREBUILT_URL` at a
mirror of `https://github.com/boyan01/lychee_player/releases/download`
(https for Swift Package Manager). On Windows and Linux,
`LYCHEE_FFMPEG_ROOT` uses a local FFmpeg instead.

## License

MIT. FFmpeg is LGPL, built by this project and linked dynamically; each
release attaches its source. Apps must ship the notices in
`core/THIRD_PARTY_NOTICES` of this package, which the prebuilt core
already carries (`lychee.framework`, and
`META-INF/lychee/THIRD_PARTY_NOTICES` in Android APKs).

Source, roadmap and the native SDKs (Swift, Kotlin, Rust):
https://github.com/boyan01/lychee_player
