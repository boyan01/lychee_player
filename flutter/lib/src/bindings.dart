// Hand-written bindings for core/include/lychee/lychee.h.

import 'dart:ffi';
import 'dart:io';

final DynamicLibrary _library = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('lychee_player.framework/lychee_player');
  }
  throw UnsupportedError('lychee_player does not support this platform yet');
}();

const int kFrameCpuBgra = 1 << 0;
const int kFrameCVPixelBuffer = 1 << 1;

const int kEventStateChanged = 1;
const int kEventPlayingChanged = 2;
const int kEventMediaInfo = 3;
const int kEventVideoSizeChanged = 4;
const int kEventSeekCompleted = 5;
const int kEventError = 6;

final class LycheeConfig extends Struct {
  @Uint32()
  external int structSize;
  @Uint32()
  external int videoFrameTypes;
  @Int32()
  external int hardwareDecoding;
}

final class LycheeStats extends Struct {
  @Uint32()
  external int structSize;
  @Int32()
  external int hardwareDecoding;
  @Int64()
  external int framesRendered;
  @Int64()
  external int framesDropped;
  @Array(32)
  external Array<Uint8> videoDecoder;
}

typedef EventCallbackNative = Void Function(
    Pointer<Void> userData, Int32 type, Int64 arg0, Int64 arg1);

final configInit = _library.lookupFunction<Void Function(Pointer<LycheeConfig>),
    void Function(Pointer<LycheeConfig>)>('lychee_config_init');

final playerCreate = _library.lookupFunction<
    Pointer<Void> Function(Pointer<LycheeConfig>),
    Pointer<Void> Function(Pointer<LycheeConfig>)>('lychee_player_create');

final playerDestroy = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_destroy');

final playerSetEventCallback = _library.lookupFunction<
    Void Function(Pointer<Void>, Pointer<NativeFunction<EventCallbackNative>>,
        Pointer<Void>),
    void Function(Pointer<Void>, Pointer<NativeFunction<EventCallbackNative>>,
        Pointer<Void>)>('lychee_player_set_event_callback');

final playerOpen = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Char>),
    int Function(Pointer<Void>, Pointer<Char>)>('lychee_player_open');

final playerPlay = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_play');

final playerPause = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_pause');

final playerSeek = _library.lookupFunction<Void Function(Pointer<Void>, Int64),
    void Function(Pointer<Void>, int)>('lychee_player_seek');

final playerSetVolume = _library.lookupFunction<
    Void Function(Pointer<Void>, Float),
    void Function(Pointer<Void>, double)>('lychee_player_set_volume');

final playerGetState = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_state');

final playerIsPlaying = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_is_playing');

final playerGetPosition = _library.lookupFunction<
    Int64 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_position');

final playerGetDuration = _library.lookupFunction<
    Int64 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_duration');

final playerGetBufferedPosition = _library.lookupFunction<
    Int64 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_buffered_position');

final playerGetVideoSize = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Int32>, Pointer<Int32>),
    int Function(Pointer<Void>, Pointer<Int32>,
        Pointer<Int32>)>('lychee_player_get_video_size');

final playerGetVideoRotation = _library.lookupFunction<
    Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_video_rotation');

final playerGetError = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Char>, Int32),
    int Function(Pointer<Void>, Pointer<Char>, int)>('lychee_player_get_error');

final playerGetStats = _library.lookupFunction<
    Void Function(Pointer<Void>, Pointer<LycheeStats>),
    void Function(Pointer<Void>, Pointer<LycheeStats>)>('lychee_player_get_stats');
