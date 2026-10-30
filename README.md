# lychee_player

[![CI](https://github.com/boyan01/lychee_player/actions/workflows/ci.yml/badge.svg)](https://github.com/boyan01/lychee_player/actions/workflows/ci.yml) [![License: MIT](https://img.shields.io/github/license/boyan01/lychee_player)](https://github.com/boyan01/lychee_player/blob/main/LICENSE)

A video and audio player for apps on macOS, iOS, Android, Windows and
Linux. It decodes on the GPU through each platform's own decoders, hands
frames to the app as native textures, and is one C library underneath a
Flutter plugin, a Swift package, an Android library and Rust crates.

> Status: beta. [docs/roadmap.md](docs/roadmap.md) lists what is missing.

## Features

- **Hardware decoding** through VideoToolbox (macOS, iOS), MediaCodec
  (Android), Media Foundation and D3D11VA (Windows) and VAAPI (Linux),
  falling back to software when the hardware gives up.
- **Formats**: MP4, Matroska/WebM, MPEG-TS and HLS without FFmpeg;
  FLV, Ogg and more with the optional LGPL FFmpeg backend.
- **HLS**: adaptive variants, fragmented MP4 segments, AES-128, alternate
  audio and subtitle renditions.
- **Playback**: accurate seeking, rate from 0.25x to 4x with pitch kept,
  gapless looping, audio, video and subtitle track selection.
- **Subtitles**: text (SRT, WebVTT, tx3g, IMSC) for the app to draw, and
  bitmap subtitles (PGS, DVB, VobSub, IMSC images) as pictures.
- **HDR** video tone-mapped to SDR, or kept as HDR on Apple displays and
  Windows HDR textures.
- **Live input**: push H.264/HEVC and AAC packets, e.g. from screen
  mirroring, and the player paces them against network jitter.

## Packages

| For | Package | Version | Get started |
| --- | --- | --- | --- |
| Flutter | [`lychee_player`](https://pub.dev/packages/lychee_player) on pub.dev | [![pub](https://img.shields.io/pub/v/lychee_player?include_prereleases)](https://pub.dev/packages/lychee_player) | [flutter/README.md](flutter/README.md) |
| Swift (macOS, iOS) | `LycheePlayer`, this repository as a Swift package | [![release](https://img.shields.io/github/v/release/boyan01/lychee_player?include_prereleases)](https://github.com/boyan01/lychee_player/releases) | [Swift](docs/native-sdks.md#swift) |
| Android | `io.github.boyan01.lychee:lychee` on Maven Central | [![Maven Central](https://img.shields.io/maven-metadata/v?metadataUrl=https%3A%2F%2Frepo1.maven.org%2Fmaven2%2Fio%2Fgithub%2Fboyan01%2Flychee%2Flychee%2Fmaven-metadata.xml&label=maven%20central)](https://central.sonatype.com/artifact/io.github.boyan01.lychee/lychee) | [Android](docs/native-sdks.md#android) |
| Rust | `lychee` and `lychee-sys` in [`rust/`](rust) | not on crates.io yet | [Rust](docs/native-sdks.md#rust) |
| C, C++ | [`lychee.h`](core/include/lychee/lychee.h) | with every release | [C ABI](docs/native-sdks.md#c-abi) |

```dart
final player = await LycheePlayer.create();
player.open('https://example.com/video.mp4');
player.play();
LycheeVideo(player); // in the widget tree
```

## Platforms

| Platform | Minimum | Hardware decoding | Without FFmpeg |
| --- | --- | --- | --- |
| macOS | 11 | VideoToolbox | Yes |
| iOS | 16 | VideoToolbox | Yes |
| Android | API 26 (arm64-v8a, armeabi-v7a, x86_64) | MediaCodec | Yes (default) |
| Windows | 10 | Media Foundation, D3D11VA | Yes |
| Linux | Ubuntu 22.04 (x86_64, arm64) | VAAPI | No, FFmpeg is required |

Without FFmpeg, each platform decodes with its own codecs: H.264 and
HEVC everywhere, VP9 and AV1 where the system has them (Windows needs
the Store's video extensions for HEVC, VP9 and AV1), AAC, MP3, Opus,
FLAC, ALAC and AC-3.
FFmpeg adds the remaining containers and codecs, and a software
decoder for anything the system lacks.

## Documentation

- [Flutter plugin](flutter/README.md): install, platform setup, FFmpeg,
  API.
- [Native SDKs](docs/native-sdks.md): Swift, Android, Rust and the C ABI.
- [Development](docs/development.md): building the core, the CLI and
  native harnesses, and checking frames and audio.
- [Releasing](docs/releasing.md): the release workflow and Maven Central.
- [Roadmap](docs/roadmap.md): what is unfinished and known defects.
- [AGENTS.md](AGENTS.md): design rules and how changes are checked, for
  contributors and coding agents.

## License

MIT, see [LICENSE](LICENSE).

FFmpeg is used under the LGPL: every shipped build is LGPL-only and
linked dynamically. An app must ship the notices in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) (Chromium, FFmpeg, dav1d),
which the prebuilt core carries (`lychee.framework`, and
`META-INF/lychee/THIRD_PARTY_NOTICES` in Android APKs).
