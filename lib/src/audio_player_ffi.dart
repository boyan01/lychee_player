import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';

import 'audio_player_common.dart';
import 'audio_player_io.dart';

DynamicLibrary _openLibrary() {
  if (Platform.isLinux) {
    return DynamicLibrary.open("libffplayer.so");
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open("ffplayer.dll");
  }
  throw UnimplementedError(
      "can not load for this library: ${Platform.localeName}");
}

final _library = _openLibrary();

final ffplayer_alloc_player =
    _library.lookupFunction<Pointer Function(), Pointer Function()>(
        "ffplayer_alloc_player");

final ffplayer_open_file = _library.lookupFunction<
    IntPtr Function(Pointer, Pointer<Utf8>),
    int Function(Pointer, Pointer<Utf8>)>("ffplayer_open_file");

final ffplayer_init = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>("ffplayer_global_init");

final ffplayer_free_player =
    _library.lookupFunction<Void Function(Pointer), void Function(Pointer)>(
        "ffplayer_free_player");

final ffplayer_get_current_position =
    _library.lookupFunction<Double Function(Pointer), double Function(Pointer)>(
        "ffplayer_get_current_position");

final ffplayer_get_duration =
    _library.lookupFunction<Double Function(Pointer), double Function(Pointer)>(
        "ffplayer_get_duration");

final ffplayer_seek_to_position = _library.lookupFunction<
    Void Function(Pointer, Double),
    void Function(Pointer, double)>("ffplayer_seek_to_position");

typedef native_buffered_callback = Void Function(
    Pointer player, Int32 what, Int64 arg1, Int64 arg2);

final ffp_set_message_callback = _library.lookupFunction<
        Void Function(
            Pointer, Int64, Pointer<NativeFunction<native_buffered_callback>>),
        void Function(
            Pointer, int, Pointer<NativeFunction<native_buffered_callback>>)>(
    "ffp_set_message_callback_dart");

var _inited = false;

void _ensureFfplayerGlobalInited() {
  if (_inited) {
    return;
  }
  _inited = true;
  ffplayer_init(NativeApi.initializeApiDLData);
}

class FfiAudioPlayer implements AudioPlayer {
  final ValueNotifier<PlayerStatus> _status = ValueNotifier(PlayerStatus.Idle);

  late Pointer player;

  late ReceivePort cppInteractPort;

  FfiAudioPlayer(String uri, DataSourceType type) {
    _ensureFfplayerGlobalInited();
    player = ffplayer_alloc_player();
    if (player == nullptr) {
      throw Exception("memory not enough");
    }

    cppInteractPort = ReceivePort("ffp: $uri")
      ..listen((message) {
        print("Dart:  on message ($message).");
      });
    ffp_set_message_callback(player, cppInteractPort.sendPort.nativePort,
        Pointer.fromFunction(_onPlayerMessageCallback));
    ffplayer_open_file(player, Utf8.toUtf8(uri));

    debugPrint("player ${player}");

  }

  static void _onPlayerMessageCallback(
      Pointer player, int what, int arg1, int arg2) {
    debugPrint("_on message: ${what} $arg1 $arg2");
  }

  @override
  bool playWhenReady = true;

  @override
  ValueListenable<List<DurationRange>> get buffered => ValueNotifier(const []);

  @override
  Duration get currentTime {
    if (player == nullptr) {
      return const Duration(microseconds: 0);
    }
    return Duration(
      milliseconds: (ffplayer_get_current_position(player) * 1000).ceil(),
    );
  }

  bool _disposed = false;

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    final player = this.player;
    if (player != nullptr) {
      this.player = nullptr;
      ffplayer_free_player(player);
    }
    cppInteractPort.close();
  }

  @override
  Duration get duration {
    if (player == nullptr) {
      return const Duration(microseconds: -1);
    }
    return Duration(
      milliseconds: (ffplayer_get_duration(player) * 1000).ceil(),
    );
  }

  @override
  ValueListenable get error => ValueNotifier(null);

  @override
  bool get hasError => error.value == null;

  @override
  bool get isPlaying => false;

  Listenable? _onStateChanged;

  @override
  Listenable get onStateChanged {
    if (_onStateChanged == null) {
      _onStateChanged = Listenable.merge([_status]);
    }
    return _onStateChanged!;
  }

  @override
  ValueListenable<PlayerStatus> get onStatusChanged => _status;

  @override
  void seek(Duration duration) {
    if (player == nullptr) {
      return;
    }
    ffplayer_seek_to_position(player, duration.inMilliseconds / 1000);
  }

  @override
  PlayerStatus get status => _status.value;
}
