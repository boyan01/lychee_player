// Hand-written bindings for core/include/lychee/lychee.h.

import 'dart:ffi';
import 'dart:io';

final DynamicLibrary _library = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('lychee_player.framework/lychee_player');
  }
  if (Platform.isAndroid) {
    // The core from the Android SDK's AAR (sdk/android).
    return DynamicLibrary.open('liblychee.so');
  }
  if (Platform.isLinux) {
    return DynamicLibrary.open('liblychee_player.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('lychee_player.dll');
  }
  throw UnsupportedError('lychee_player does not support this platform');
}();

const int kFrameCpuBgra = 1 << 0;
const int kFrameCVPixelBuffer = 1 << 1;
const int kFrameCpuRgba = 1 << 2;
const int kFrameAndroidSurface = 1 << 3;
const int kFrameD3D11Texture = 1 << 4;
const int kFrameDmabuf = 1 << 5;

/// The frame types the platform's texture bridge consumes: hardware frames,
/// and CPU frames when hardware decoding rejects a stream.
int get platformFrameType {
  if (Platform.isMacOS || Platform.isIOS) {
    return kFrameCVPixelBuffer;
  }
  if (Platform.isAndroid) {
    return kFrameAndroidSurface | kFrameCpuRgba;
  }
  if (Platform.isWindows) {
    // D3D11 frames only come once the plugin has passed Flutter's adapter.
    // BGRA, not RGBA: ANGLE opens shared textures only in BGRA.
    return kFrameD3D11Texture | kFrameCpuBgra;
  }
  if (Platform.isLinux) {
    // Dmabuf frames only come once the plugin has found that Flutter's EGL
    // display imports them.
    return kFrameDmabuf | kFrameCpuRgba;
  }
  return kFrameCpuRgba;
}

const int kEventStateChanged = 1;
const int kEventPlayingChanged = 2;
const int kEventMediaInfo = 3;
const int kEventVideoSizeChanged = 4;
const int kEventSeekCompleted = 5;
const int kEventError = 6;
const int kEventTracksChanged = 7;
const int kEventSubtitleChanged = 8;

const int kTrackVideo = 1;
const int kTrackAudio = 2;
const int kTrackSubtitle = 3;

final class LycheeConfig extends Struct {
  @Uint32()
  external int structSize;
  @Uint32()
  external int videoFrameTypes;
  @Int32()
  external int hardwareDecoding;
  // Set by the plugin's native side, not from Dart.
  external Pointer<Void> videoDevice;
  // Unused: Flutter rotates the texture widget. Declared because
  // lychee_config_init() writes the whole struct.
  @Int32()
  external int rotateSurfaceFrames;
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
  @Int64()
  external int videoBufferedUs;
  @Int64()
  external int audioBufferedUs;
  @Int64()
  external int videoDecodeTimeUs;
}

final class LycheeTrackInfo extends Struct {
  @Uint32()
  external int structSize;
  @Int32()
  external int type;
  @Int32()
  external int id;
  @Int32()
  external int selected;
  @Array(32)
  external Array<Uint8> codec;
  @Array(16)
  external Array<Uint8> language;
  @Array(128)
  external Array<Uint8> title;
  @Int32()
  external int width;
  @Int32()
  external int height;
  @Int32()
  external int sampleRate;
  @Int32()
  external int channels;
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

final playerGetPosition = _library.lookupFunction<Int64 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_position');

final playerGetDuration = _library.lookupFunction<Int64 Function(Pointer<Void>),
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
    void Function(
        Pointer<Void>, Pointer<LycheeStats>)>('lychee_player_get_stats');

final playerSetRate = _library.lookupFunction<
    Void Function(Pointer<Void>, Double),
    void Function(Pointer<Void>, double)>('lychee_player_set_rate');

final playerGetRate = _library.lookupFunction<Double Function(Pointer<Void>),
    double Function(Pointer<Void>)>('lychee_player_get_rate');

final playerSetLooping = _library.lookupFunction<
    Void Function(Pointer<Void>, Int32),
    void Function(Pointer<Void>, int)>('lychee_player_set_looping');

final playerIsLooping = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_is_looping');

final playerGetTrackCount = _library.lookupFunction<
    Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_get_track_count');

final playerGetTrackInfo = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Int32, Pointer<LycheeTrackInfo>),
    int Function(Pointer<Void>, int,
        Pointer<LycheeTrackInfo>)>('lychee_player_get_track_info');

final playerSelectTrack = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Int32, Int32),
    int Function(Pointer<Void>, int, int)>('lychee_player_select_track');

final playerGetSubtitle = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Int64, Pointer<Char>, Int32),
    int Function(
        Pointer<Void>, int, Pointer<Char>, int)>('lychee_player_get_subtitle');

final playerGetMetadata = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Char>, Pointer<Char>, Int32),
    int Function(Pointer<Void>, Pointer<Char>, Pointer<Char>,
        int)>('lychee_player_get_metadata');

final playerGetArtwork = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32),
    int Function(
        Pointer<Void>, Pointer<Uint8>, int)>('lychee_player_get_artwork');

final class LycheeSubtitleImage extends Struct {
  @Uint32()
  external int structSize;
  @Int32()
  external int x;
  @Int32()
  external int y;
  @Int32()
  external int width;
  @Int32()
  external int height;
  @Int32()
  external int bitmapWidth;
  @Int32()
  external int bitmapHeight;
  @Int32()
  external int stride;
  external Pointer<Uint8> data;
}

final playerAcquireSubtitle = _library.lookupFunction<
    Pointer<Void> Function(Pointer<Void>, Int64, Int32, Int32),
    Pointer<Void> Function(
        Pointer<Void>, int, int, int)>('lychee_player_acquire_subtitle');

final subtitleRelease = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_subtitle_release');

final subtitleGetId = _library.lookupFunction<Int64 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_subtitle_get_id');

final subtitleGetImageCount = _library.lookupFunction<
    Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_subtitle_get_image_count');

final subtitleGetImage = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Int32, Pointer<LycheeSubtitleImage>),
    int Function(Pointer<Void>, int,
        Pointer<LycheeSubtitleImage>)>('lychee_subtitle_get_image');
