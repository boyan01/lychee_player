import 'dart:async';
import 'dart:ffi' hide Size;

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'bindings.dart' as native;

/// Matches lychee_state.
enum LycheePlayerState { idle, loading, ready, buffering, ended, error }

@immutable
class LycheePlayerValue {
  const LycheePlayerValue({
    this.state = LycheePlayerState.idle,
    this.playing = false,
    this.position = Duration.zero,
    this.duration,
    this.buffered = Duration.zero,
    this.videoSize,
    this.rotation = 0,
    this.error,
  });

  final LycheePlayerState state;

  /// Whether playback is requested; it actually advances only when [state]
  /// is [LycheePlayerState.ready].
  final bool playing;
  final Duration position;

  /// Null for live streams or before the media is loaded.
  final Duration? duration;
  final Duration buffered;

  /// Display size before rotation; null if there is no video.
  final Size? videoSize;

  /// Clockwise rotation in degrees.
  final int rotation;
  final String? error;
}

/// Playback statistics, read on demand with [LycheePlayer.getStats].
@immutable
class LycheePlayerStats {
  const LycheePlayerStats({
    this.hardwareDecoding = false,
    this.framesRendered = 0,
    this.framesDropped = 0,
    this.videoDecoder = '',
  });

  final bool hardwareDecoding;

  /// Frames handed to the display.
  final int framesRendered;

  /// Decoded frames skipped because a later one was due.
  final int framesDropped;
  final String videoDecoder;
}

class LycheePlayer extends ValueNotifier<LycheePlayerValue> {
  LycheePlayer._(this._player, this._events, this.textureId)
      : super(const LycheePlayerValue());

  static const _channel = MethodChannel('lychee_player');

  final Pointer<Void> _player;
  final NativeCallable<native.EventCallbackNative> _events;
  final int textureId;
  Timer? _ticker;
  bool _disposed = false;

  static Future<LycheePlayer> create({bool hardwareDecoding = true}) async {
    final config = calloc<native.LycheeConfig>();
    native.configInit(config);
    config.ref.videoFrameTypes = native.kFrameCVPixelBuffer;
    config.ref.hardwareDecoding = hardwareDecoding ? 1 : 0;
    final player = native.playerCreate(config);
    calloc.free(config);

    late final LycheePlayer instance;
    final events = NativeCallable<native.EventCallbackNative>.listener(
        (Pointer<Void> _, int type, int arg0, int arg1) =>
            instance._onEvent(type));
    native.playerSetEventCallback(player, events.nativeFunction, nullptr);
    final textureId = await _channel.invokeMethod<int>(
        'createTexture', {'player': player.address});
    instance = LycheePlayer._(player, events, textureId!);
    instance._ticker = Timer.periodic(
        const Duration(milliseconds: 200), (_) => instance._onTick());
    return instance;
  }

  // Calls after dispose() are ignored: the native player may already be
  // destroyed, and widgets can still hold this object until their next build.
  void open(String url) {
    if (_disposed) {
      return;
    }
    final nativeUrl = url.toNativeUtf8();
    native.playerOpen(_player, nativeUrl.cast());
    malloc.free(nativeUrl);
    _refresh();
  }

  void play() {
    if (!_disposed) native.playerPlay(_player);
  }

  void pause() {
    if (!_disposed) native.playerPause(_player);
  }

  void seek(Duration position) {
    if (!_disposed) native.playerSeek(_player, position.inMicroseconds);
  }

  void setVolume(double volume) {
    if (!_disposed) native.playerSetVolume(_player, volume);
  }

  LycheePlayerStats getStats() {
    if (_disposed) {
      return const LycheePlayerStats();
    }
    final stats = calloc<native.LycheeStats>();
    stats.ref.structSize = sizeOf<native.LycheeStats>();
    native.playerGetStats(_player, stats);
    final decoder = <int>[];
    for (var i = 0; i < 32 && stats.ref.videoDecoder[i] != 0; i++) {
      decoder.add(stats.ref.videoDecoder[i]);
    }
    final result = LycheePlayerStats(
      hardwareDecoding: stats.ref.hardwareDecoding != 0,
      framesRendered: stats.ref.framesRendered,
      framesDropped: stats.ref.framesDropped,
      videoDecoder: String.fromCharCodes(decoder),
    );
    calloc.free(stats);
    return result;
  }

  void _onEvent(int type) => _refresh();

  void _onTick() {
    final state = value.state;
    if (value.playing ||
        state == LycheePlayerState.loading ||
        state == LycheePlayerState.buffering) {
      _refresh();
    }
  }

  void _refresh() {
    if (_disposed) {
      return;
    }
    final state = LycheePlayerState.values[native.playerGetState(_player)];
    final duration = native.playerGetDuration(_player);

    Size? videoSize;
    final width = calloc<Int32>();
    final height = calloc<Int32>();
    if (native.playerGetVideoSize(_player, width, height) != 0) {
      videoSize = Size(width.value.toDouble(), height.value.toDouble());
    }
    calloc.free(width);
    calloc.free(height);

    String? error;
    if (state == LycheePlayerState.error) {
      final buffer = calloc<Char>(512);
      native.playerGetError(_player, buffer, 512);
      error = buffer.cast<Utf8>().toDartString();
      calloc.free(buffer);
    }

    value = LycheePlayerValue(
      state: state,
      playing: native.playerIsPlaying(_player) != 0,
      position: Duration(microseconds: native.playerGetPosition(_player)),
      duration: duration < 0 ? null : Duration(microseconds: duration),
      buffered:
          Duration(microseconds: native.playerGetBufferedPosition(_player)),
      videoSize: videoSize,
      rotation: native.playerGetVideoRotation(_player),
      error: error,
    );
  }

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _ticker?.cancel();
    super.dispose();
    _release();
  }

  Future<void> _release() async {
    // The texture must stop pulling frames before the player goes away, and
    // the player must stop calling back before the callable is closed.
    await _channel.invokeMethod('disposeTexture', {'textureId': textureId});
    native.playerDestroy(_player);
    _events.close();
  }
}
