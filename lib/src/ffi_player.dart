import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';

import 'audio_player_common.dart';
import 'audio_player_platform.dart';
import 'extension/change_notifier.dart';

class _PlayerConfiguration extends Struct {
  @Int32()
  external int audio_disable;
  @Int32()
  external int video_disable;
  @Int32()
  external int subtitle_disable;
  @Int32()
  external int seek_by_bytes;
  @Int32()
  external int show_status;

  @Int64()
  external int start_time;

  @Int32()
  external int loop;

  static Pointer<_PlayerConfiguration> alloctConfiguration() {
    final pointer = calloc<_PlayerConfiguration>();
    pointer.ref
      ..video_disable = 0
      ..audio_disable = 0
      ..subtitle_disable = 1
      ..start_time = 0
      ..loop = 1
      ..show_status = 0;
    return pointer;
  }
}

DynamicLibrary _openLibrary() {
  if (Platform.isAndroid) {
    return DynamicLibrary.open("libmedia_flutter.so");
  }
  if (Platform.isLinux) {
    return DynamicLibrary.open("libmedia_flutter.so");
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open("media_flutter.dll");
  }
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.process();
  }
  throw UnimplementedError(
      "can not load for this library: ${Platform.localeName}");
}

final _library = _openLibrary();

final ffp_create_player = _library.lookupFunction<
    Pointer Function(Pointer<_PlayerConfiguration>),
    Pointer Function(Pointer<_PlayerConfiguration>)>("ffp_create_player");

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

final ffp_set_message_callback = _library.lookupFunction<
    Void Function(Pointer, Int64),
    void Function(Pointer, int)>("ffp_set_message_callback_dart");

final ffplayer_is_paused =
    _library.lookupFunction<Int8 Function(Pointer), int Function(Pointer)>(
        "ffplayer_is_paused");

final media_set_play_when_ready = _library.lookupFunction<
    Void Function(Pointer, Int8),
    void Function(Pointer, int)>("media_set_play_when_ready");

final ffp_attach_video_render_flutter =
    _library.lookupFunction<Int64 Function(Pointer), int Function(Pointer)>(
        "ffp_attach_video_render_flutter");

final ffp_detach_video_render_flutter =
    _library.lookupFunction<Void Function(Pointer), void Function(Pointer)>(
        "ffp_detach_video_render_flutter");

final ffp_get_volume =
    _library.lookupFunction<Double Function(Pointer), double Function(Pointer)>(
        "ffp_get_volume");

final ffp_set_volume = _library.lookupFunction<Void Function(Pointer, Double),
    void Function(Pointer, double)>("ffp_set_volume");

final ffp_get_video_aspect_ratio =
    _library.lookupFunction<Double Function(Pointer), double Function(Pointer)>(
        "ffp_get_video_aspect_ratio");

var _inited = false;

void _ensureFfplayerGlobalInited() {
  if (_inited) {
    return;
  }
  _inited = true;
  ffplayer_init(NativeApi.initializeApiDLData);
}

class LycheePlayer implements AudioPlayer {
  final ValueNotifier<PlayerStatus> _status = ValueNotifier(PlayerStatus.idle);
  final _buffred = ValueNotifier<List<DurationRange>>(const []);
  final _error = ValueNotifier(null);
  final _videoSize = ValueNotifier(Size.zero);

  late Pointer _player;

  late ReceivePort cppInteractPort;

  LycheePlayer(String uri, DataSourceType type) {
    _ensureFfplayerGlobalInited();
    final configuration = _PlayerConfiguration.alloctConfiguration();
    _player = ffp_create_player(configuration);
    if (_player == nullptr) {
      throw Exception("memory not enough");
    }

    cppInteractPort = ReceivePort("ffp: $uri")
      ..listen((message) {
        assert(message is Uint8List);
        final values = Int64List.view(message.buffer);
        debugPrint('values[0], values[1], values[2]');
      });
    ffp_set_message_callback(_player, cppInteractPort.sendPort.nativePort);
    ffplayer_open_file(_player, uri.toNativeUtf8());

    // FIXME remove this lately.
    _status.value = PlayerStatus.ready;
  }

  final _playWhenReady = ValueNotifier<bool>(false);

  @override
  bool get playWhenReady => _playWhenReady.value;

  @override
  set playWhenReady(bool value) {
    if (_player == nullptr) {
      return;
    }
    _playWhenReady.value = value;
    media_set_play_when_ready(_player, value ? 1 : 0);
  }

  @override
  ValueListenable<List<DurationRange>> get buffered => _buffred;

  @override
  Duration get currentTime {
    if (_player == nullptr) {
      return const Duration();
    }
    final seconds = ffplayer_get_current_position(_player);
    if (seconds.isNaN) {
      return const Duration();
    }
    return Duration(
      milliseconds: (seconds * 1000).ceil(),
    );
  }

  bool _disposed = false;

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _status.value = PlayerStatus.idle;
    if (_player != nullptr) {
      ffplayer_free_player(_player);
      _player = nullptr;
    }
    _renderAttached = 0;
    cppInteractPort.close();
  }

  @override
  Duration get duration {
    if (_player == nullptr) {
      return const Duration(microseconds: -1);
    }
    return Duration(
      milliseconds: (ffplayer_get_duration(_player) * 1000).ceil(),
    );
  }

  @override
  double get volume {
    if (_player == nullptr) {
      return 0;
    }
    return ffp_get_volume(_player);
  }

  @override
  set volume(double volume) {
    if (_player == nullptr) {
      return;
    }
    ffp_set_volume(_player, volume);
  }

  ValueNotifier<double>? _aspectRatio;

  ValueNotifier<double> get aspectRatio {
    assert(_player != nullptr);
    _aspectRatio ??= _videoSize.map((size) {
      if (size == Size.zero) {
        return 0.0;
      }
      return size.aspectRatio;
    });
    return _aspectRatio!;
  }

  @override
  ValueListenable get error => _error;

  @override
  bool get hasError => _error.value != null;

  @override
  bool get isPlaying => playWhenReady && _status.value == PlayerStatus.ready;

  Listenable? _onStateChanged;

  @override
  Listenable get onStateChanged {
    _onStateChanged ??= Listenable.merge([_status, _playWhenReady]);
    return _onStateChanged!;
  }

  @override
  ValueListenable<PlayerStatus> get onStatusChanged => _status;

  @override
  void seek(Duration duration) {
    if (_player == nullptr) {
      return;
    }
    ffplayer_seek_to_position(_player, duration.inMilliseconds / 1000);
  }

  @override
  PlayerStatus get status => _status.value;

  int _renderAttached = 0;

  int attachVideoRender() {
    assert(_player != nullptr);
    _renderAttached++;
    return ffp_attach_video_render_flutter(_player);
  }

  void detachVideoRender() {
    if (_disposed) {
      return;
    }
    assert(_player != nullptr);
    _renderAttached--;
    if (_renderAttached <= 0) {
      ffp_detach_video_render_flutter(_player);
    }
  }
}
