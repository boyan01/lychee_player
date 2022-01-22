# lychee_player

A simple audio/video player example for flutter.

![img](preview/preview.png)

This is my training project when I was learning C++.

| platform | status       |  audio renderer | video renderer |
| -------- | ------------ | --------------  | -------------- |
| Windows  | ✅           | [SDL2](https://github.com/libsdl-org/SDL)           |   TextureWidget |
| Linux    | ✅           | [SDL2](https://github.com/libsdl-org/SDL)           |   TextureWidget |
| macOS    | ✅           | CoreAudio           |   TextureWidget |
| Android  | ✅           | [Oboe](https://github.com/google/oboe)           |   TextureWidget |
| iOS      | ✅           | CoreAudio           |   TextureWidget |

* audio/video demux by ffmpeg.

## Getting Started

### How to Build Project?

#### requirement:

* flutter version: 2.5 (linux required latest master channel)
* if build for linux, we need these libs:
    1. install ffmpeg dev libs:
       ```
       sudo apt install libavcodec-dev libavformat-dev libavdevice-dev
       ```
    2. install sdl2:
       ```
       sudo apt-get install libsdl2-dev
       ```

#### Build for Flutter

* `flutter pub get`.
* if build for macos/ios
    1. go to `example/macos` or `example/ios` run `pod install` to install `ffmpeg-kit`
    2. go to `ffplayer`, run `./apple-flutter-install.sh macos` or `./apple-flutter-install.sh ios`

* `flutter run -d your_device`

## Dev Tips

### how to debug c/c++ code if we build for flutter ?

#### Windows

1. run windows app.

```shell
flutter run -d windows
```

2. open `example/build/windows/lychee_player_example.sln` by visual studio 2019
3. mark `lychee_player_example` as run program. (which ALL_BUILD is default selected, but we can not run it).
4. click run with local debug. then waiting for crash.

##### Linux

1. add remote gdb debug configuration. target remote args set

```
127.0.0.1:1234
```

2. run the application which flutter build with debug.

```shell
gdbserver :1234 build/linux/debug/bundle/lychee_player_example
```

# LICENSE

[AGPL-v3](LICENSE)
