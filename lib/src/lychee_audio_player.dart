import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';

import 'lychee_player_bindings_generated.dart';

enum PlayerState {
  idle,
  buffering,
  ready,
  end,
}

class LycheeAudioPlayer {
  LycheeAudioPlayer(this.url)
      : _port = ReceivePort('lychee_player_port: $url') {
    _initializeDartApi();
    _playerHandle = _bindings.lychee_player_create(
        url.toNativeUtf8().cast(), _port.sendPort.nativePort);
    _port.listen((message) {
      assert(message is Uint8List);
      final values = Int64List.view(message.buffer);
      assert(values.length == 3);
      _handleNativePlayerMessage(values[0], values[1], values[2]);
    });
  }

  final String url;

  Pointer<Void> _playerHandle = nullptr;
  final ReceivePort _port;
  final ValueNotifier<PlayerState> _state = ValueNotifier(PlayerState.idle);

  final _playWhenReady = ValueNotifier(false);

  void _handleNativePlayerMessage(int what, int arg1, int arg2) {
    debugPrint('what: $what, arg1: $arg1, arg2: $arg2');
    switch (what) {
      case MSG_PLAYER_STATE_CHANGED:
        _state.value = const {
          0: PlayerState.idle,
          1: PlayerState.ready,
          2: PlayerState.buffering,
          3: PlayerState.end,
        }[arg1]!;
        break;
    }
  }

  Listenable get onPlayWhenReadyChanged => _playWhenReady;

  ValueListenable<PlayerState> get state => _state;

  bool get playWhenReady => _playWhenReady.value;

  set playWhenReady(bool value) {
    _playWhenReady.value = value;
    assert(_playerHandle != nullptr, 'player already disposed.');
    _bindings.lychee_player_set_play_when_ready(_playerHandle, value);
  }

  double currentTime() {
    assert(_playerHandle != nullptr, 'player already disposed.');
    return _bindings.lychee_player_get_current_time(_playerHandle);
  }

  double duration() {
    assert(_playerHandle != nullptr, 'player already disposed.');
    return _bindings.lychee_player_get_duration(_playerHandle);
  }

  void dispose() {
    _bindings.lychee_player_dispose(_playerHandle);
    _playerHandle = nullptr;
  }

  void seek(double time) {
    assert(_playerHandle != nullptr, 'player already disposed.');
    _bindings.lychee_player_seek(_playerHandle, time);
  }
}

bool _isolateInitialized = false;

void _initializeDartApi() {
  if (_isolateInitialized) {
    return;
  }
  _isolateInitialized = true;
  _bindings.lychee_player_initialize_dart(NativeApi.initializeApiDLData);
}

const String _libName = 'lychee_player';

final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.process();
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('lychee_player_plugin.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

/// The bindings to the native functions in [_dylib].
final LycheePlayerBindings _bindings = LycheePlayerBindings(_dylib);
