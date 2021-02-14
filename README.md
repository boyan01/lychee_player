# av_player

A simple audio/video player example for flutter. based on [ffplay](http://ffmpeg.org/).

![img](preview/preview.png)

This is my training project when I was learning C++.

* audio/video demux by ffmpeg.

* audio render by SDL2.

* video render by SDL2(example) / `TextureWidget`(flutter).

| platform | status       |
| -------- | ------------ |
| Windows  | ✅            |
| Linux    | ⭕ audio only |
| macOS    | ⭕ audio only |
| Android  | ⭕ audio only |
| iOS      | ⭕ audio only |

## Getting Started

### How to Build Projext?

1. set up your enviorenment. see more [ffplayer](ffplayer/README.md)
2. install flutter.
3. `cd example & flutter run`

## Dev Tips

### Linux

#### how to debug c/c++ code in Clion

1. add remote gdb debug configuration. target remote args set

```
127.0.0.1:1234
```

2. run the application which flutter build with debug.

```shell
gdbserver :1234 build/linux/debug/bundle/audio_player_example
```

### Windows

#### how to debug c/c++ code if we got a crash.

1. run windows app.

```shell
flutter run -d windows
```

2. open `example/build/windows/audio_player_example.sln` by visual studio 2019
3. mark `audio_player_example` as run program. (which ALL_BUILD is default selected, but we can not run it).
4. click run with local debug. then waiting for crash.

# LICENSE

[MIT](LICENSE)
