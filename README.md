# audio_player

A new flutter plugin project.

## Getting Started

This project is a starting point for a Flutter
[plug-in package](https://flutter.dev/developing-packages/),
a specialized package that includes platform-specific implementation code for
Android and/or iOS.

For help getting started with Flutter, view our
[online documentation](https://flutter.dev/docs), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

The plugin project was generated without specifying the `--platforms` flag, no platforms are currently supported.
To add platforms, run `flutter create -t plugin --platforms <platforms> .` under the same
directory. You can also find a detailed instruction on how to add platforms in the `pubspec.yaml` at https://flutter.dev/docs/development/packages-and-plugins/developing-packages#plugin-platforms.


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
