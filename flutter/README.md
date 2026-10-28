# lychee_player

A video and audio player for Flutter with hardware decoding, on a reusable
C/C++ core: VideoToolbox on macOS and iOS, MediaCodec on Android, Media
Foundation and D3D11VA on Windows, VAAPI on Linux, and an LGPL-only FFmpeg for everything else.
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
| Android | API 26, 64-bit | `minSdk = 26` and the ABIs below in `android/app/build.gradle.kts` |
| iOS | 16.0 | `platform :ios, '16.0'` in `ios/Podfile` and the Runner's deployment target |
| macOS | 11.0 | `platform :osx, '11.0'` in `macos/Podfile` and the Runner's deployment target |
| Windows | 10 | Visual Studio 2022 |
| Linux | Ubuntu 22.04 | `libepoxy-dev` and `libpulse-dev` to build; at run time FFmpeg loads the system's `libva2`, `libva-drm2`, `libdrm2`, `libgnutls30` and `zlib1g` |

Hardware decoding: VideoToolbox, MediaCodec, Media Foundation and D3D11VA
have been seen working, and VAAPI on Linux with an Intel GPU.

The Android core is built for `arm64-v8a` and `x86_64` only. Otherwise
Flutter also packs a 32-bit engine, and on a 32-bit device (or a 64-bit
one running a 32-bit system, as some TV boxes do) the app starts but
`LycheePlayer.create()` throws. In `defaultConfig` (the Flutter Gradle
plugin has set its own list there already):

```kotlin
if (project.findProperty("split-per-abi") != "true") {
    ndk {
        abiFilters.clear()
        abiFilters += listOf("arm64-v8a", "x86_64")
    }
}
```

Split APKs use a separate ABI configuration, which cannot be combined
with `ndk.abiFilters`. Select the supported targets on the command line:

```sh
flutter build apk --split-per-abi --target-platform=android-arm64,android-x64
```

On macOS, Flutter's template runs the app in the App Sandbox: add
`com.apple.security.network.client` to both entitlements files to play
URLs, and open local files the user picked (`file_selector`) or that live
in the app's container; other paths fail with "Operation not permitted".

On Linux with NVIDIA's driver, an app that quits while a video plays can
crash in `libnvidia-eglcore`: Flutter's template leaves its window, and so
the engine's raster thread, alive into `exit()`, where NVIDIA's EGL tears
itself down. Destroy the window in the runner's `shutdown`, as the
example's `linux/runner/my_application.cc` does (keep the window from
`activate` in the application struct).

The native core comes prebuilt for Android, iOS and macOS: the first build
downloads it from this version's GitHub release into `~/.cache/lychee_player`
and checks each file against the SHA-256 the package pins. Where GitHub is
slow or blocked, point `LYCHEE_PREBUILT_URL` at a mirror of
`https://github.com/boyan01/lychee_player/releases/download` (the files
are checked the same way). Windows and Linux compile the core with the app
and download an LGPL FFmpeg from the same release, through the same mirror;
`LYCHEE_FFMPEG_ROOT` names a local one instead. On Windows,
`LYCHEE_WITH_FFMPEG=OFF` builds the core without FFmpeg (Media Foundation:
H.264, HEVC, AAC, MP3; MP4, Matroska, MPEG-TS and HLS), and the app ships
no FFmpeg DLLs.

## License

MIT. The FFmpeg libraries it ships are LGPL and dynamically linked.
`core/THIRD_PARTY_NOTICES` in this package describes their sources and
builds by platform: all use this project's builds, with source attached
to each release. An app must include the notices for the libraries it
ships. The prebuilt core
carries this project's notice into apps (`lychee.framework`, and
`META-INF/lychee/THIRD_PARTY_NOTICES` in Android APKs).

Source, roadmap and the native SDKs (Swift, Kotlin, Rust):
https://github.com/boyan01/lychee_player
