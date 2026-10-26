# lychee_player

A video and audio player for Flutter with hardware decoding, on a reusable
C/C++ core: VideoToolbox on macOS and iOS, MediaCodec on Android, D3D11VA
on Windows, VAAPI on Linux, and an LGPL-only FFmpeg for everything else.
Frames reach Flutter as textures without a copy where the platform allows.

```dart
import 'package:lychee_player/lychee_player.dart';

final player = await LycheePlayer.create();
player.open('https://example.com/video.mp4');
player.play();

// In a widget tree; rebuilds as the player's value changes.
LycheeVideo(player);

// player.value: state, position, duration, tracks, subtitle, ...
// player.seek(), setRate(), setVolume(), setLooping(), selectTrack()
player.dispose();
```

## Platforms

| Platform | Minimum | Set in your app |
|---|---|---|
| Android | API 26 | `minSdk = 26` in `android/app/build.gradle.kts` |
| iOS | 16.0 | `platform :ios, '16.0'` in `ios/Podfile` and the Runner's deployment target |
| macOS | 11.0 | `platform :osx, '11.0'` in `macos/Podfile` and the Runner's deployment target |
| Windows | 10 | Visual Studio 2022 |
| Linux | Ubuntu 22.04 | `libepoxy-dev` and `libpulse-dev` to build |

On macOS, Flutter's template runs the app in the App Sandbox: add
`com.apple.security.network.client` to both entitlements files to play
URLs, and open local files the user picked (`file_selector`) or that live
in the app's container; other paths fail with "Operation not permitted".

The native core comes prebuilt for Android, iOS and macOS: the first build
downloads it from this version's GitHub release into `~/.cache/lychee_player`
and checks each file against the SHA-256 the package pins. Where GitHub is
slow or blocked, point `LYCHEE_PREBUILT_URL` at a mirror of
`https://github.com/boyan01/lychee_player/releases/download` (the files
are checked the same way). Windows and Linux compile the core with the app
and download an LGPL FFmpeg; `LYCHEE_FFMPEG_ROOT` names a local one instead.

## License

MIT. The FFmpeg libraries it ships are LGPL and dynamically linked; their
source is attached to each release. An app that ships them must say so in
its notices: `core/THIRD_PARTY_NOTICES` in this package has the text to
show (FFmpeg, Mbed TLS, code from Chromium, and their licenses), and the
prebuilt core carries it into apps (`lychee.framework`, and
`META-INF/lychee/THIRD_PARTY_NOTICES` in Android APKs).

Source, roadmap and the native SDKs (Swift, Kotlin, Rust):
https://github.com/boyan01/lychee_player
