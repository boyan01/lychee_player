// Hand-written bindings for core/include/lychee/lychee.h.

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

final DynamicLibrary _library = () {
  if (Platform.isMacOS || Platform.isIOS) {
    // The core's own framework, which the plugin links: with Swift Package
    // Manager the plugin is a static library in the app, with no framework
    // of its own.
    return DynamicLibrary.open('lychee.framework/lychee');
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
const int kEventSubtitleAdded = 9;
const int kEventVariantChanged = 10;
const int kEventItemChanged = 11;

const int kTrackVideo = 1;
const int kTrackAudio = 2;
const int kTrackSubtitle = 3;

const int kErrorNone = 0;
const int kErrorOther = 1;
const int kErrorNetwork = 2;
const int kErrorHttp = 3;
const int kErrorFile = 4;
const int kErrorFormat = 5;
const int kErrorCodec = 6;
const int kErrorDecode = 7;
const int kErrorEndedEarly = 8;

const int kTrackRoleMain = 0;
const int kTrackRoleCommentary = 1;
const int kTrackRoleDescription = 2;
const int kTrackRoleHearingImpaired = 3;

final class LycheeConfig extends Struct {
  @Uint32()
  external int structSize;
  @Uint32()
  external int videoFrameTypes;
  @Int32()
  external int hardwareDecoding;
  // Set by the plugin's native side, not from Dart.
  external Pointer<Void> videoDevice;
  // Unused: Flutter rotates the texture widget.
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
  @Array(32)
  external Array<Uint8> audioDecoder;
  @Int64()
  external int liveVideoPacketsDropped;
  @Int64()
  external int liveAudioLateUs;
  @Int64()
  external int liveAudioSilenceUs;
  @Int64()
  external int liveAudioBehindUs;
  @Int64()
  external int framesDecoded;
  @Int64()
  external int videoBytesRead;
  @Int64()
  external int audioBytesRead;
  @Int64()
  external int downloadRateBps;
  @Int64()
  external int startupTimeUs;
  @Int64()
  external int rebufferCount;
  @Int64()
  external int rebufferTimeUs;
  @Int64()
  external int seekCount;
  @Int64()
  external int lastSeekTimeUs;
  @Int64()
  external int videoOffsetUs;
  @Int32()
  external int audioOutputSampleRate;
  @Int32()
  external int audioOutputChannels;
  @Int64()
  external int audioOutputLatencyUs;
  @Int64()
  external int variantBandwidthBps;
  @Double()
  external double videoFrameRate;
  @Int32()
  external int videoColorTransfer;
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
  // lychee_track_info.external (API 17); an older core leaves it 0.
  @Int32()
  external int isExternal;
  // API 19; an older core leaves them 0.
  @Int32()
  external int isDefault;
  @Int32()
  external int forced;
  @Int32()
  external int role;
  @Int64()
  external int bitrate;
}

final class LycheeVariantInfo extends Struct {
  @Uint32()
  external int structSize;
  @Int64()
  external int bandwidth;
  @Int32()
  external int width;
  @Int32()
  external int height;
  @Double()
  external double frameRate;
  @Array(64)
  external Array<Uint8> codecs;
  @Int32()
  external int track;
  @Int32()
  external int playing;
  @Int32()
  external int selected;
}

typedef EventCallbackNative = Void Function(
    Pointer<Void> userData, Int32 type, Int64 arg0, Int64 arg1);

final configInit = _library.lookupFunction<
    Void Function(Pointer<LycheeConfig>, Uint32),
    void Function(Pointer<LycheeConfig>, int)>('lychee_config_init');

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

/// lychee_player_clear_event_callback, for a NativeFinalizer: an isolate
/// runs its native finalizers when it shuts down (a hot restart), before it
/// deletes its NativeCallables.
final playerClearEventCallbackPointer =
    _library.lookup<NativeFunction<Void Function(Pointer<Void>)>>(
        'lychee_player_clear_event_callback');

final playerOpen = _library.lookupFunction<
    Int32 Function(Pointer<Void>, Pointer<Char>),
    int Function(Pointer<Void>, Pointer<Char>)>('lychee_player_open');

/// lychee_api_version().
final apiVersion = _library
    .lookupFunction<Int32 Function(), int Function()>('lychee_api_version');

/// lychee_open_options (API 21; the audio URLs since API 29).
final class LycheeOpenOptions extends Struct {
  @Uint32()
  external int structSize;
  external Pointer<Char> headers;
  @Int32()
  external int audioUrlCount;
  external Pointer<Pointer<Char>> audioUrls;
}

/// Null with a core older than API 21.
final int Function(Pointer<Void>, Pointer<Char>, Pointer<LycheeOpenOptions>)?
    playerOpenWithOptions =
    _library.providesSymbol('lychee_player_open_with_options')
        ? _library.lookupFunction<
            Int32 Function(
                Pointer<Void>, Pointer<Char>, Pointer<LycheeOpenOptions>),
            int Function(Pointer<Void>, Pointer<Char>,
                Pointer<LycheeOpenOptions>)>('lychee_player_open_with_options')
        : null;

/// [headers] as the "Name: value" lines the C API takes.
String headerLines(Map<String, String>? headers) {
  final lines = StringBuffer();
  headers?.forEach((name, value) {
    if (name.isEmpty ||
        name.contains(RegExp('[\r\n:]')) ||
        value.contains(RegExp('[\r\n]'))) {
      throw ArgumentError.value(name, 'headers', 'malformed HTTP header');
    }
    lines.write('$name: $value\r\n');
  });
  return lines.toString();
}

/// Calls [call] with lychee_open_options holding the header [lines] and
/// [audioUrls] (null for neither), freed after it returns.
T withOpenOptions<T>(String lines, List<String>? audioUrls,
    T Function(Pointer<LycheeOpenOptions>) call) {
  final audio = audioUrls ?? const <String>[];
  if (lines.isEmpty && audio.isEmpty) {
    return call(nullptr);
  }
  final options = calloc<LycheeOpenOptions>();
  final nativeHeaders = lines.toNativeUtf8();
  final nativeAudio = calloc<Pointer<Char>>(audio.length);
  for (var i = 0; i < audio.length; i++) {
    nativeAudio[i] = audio[i].toNativeUtf8().cast();
  }
  options.ref
    ..structSize = sizeOf<LycheeOpenOptions>()
    ..headers = nativeHeaders.cast()
    ..audioUrlCount = audio.length
    ..audioUrls = nativeAudio;
  try {
    return call(options);
  } finally {
    for (var i = 0; i < audio.length; i++) {
      malloc.free(nativeAudio[i]);
    }
    calloc.free(nativeAudio);
    malloc.free(nativeHeaders);
    calloc.free(options);
  }
}

/// Null with a core older than API 23.
final int Function(Pointer<Void>)? playerClose =
    _library.providesSymbol('lychee_player_close')
        ? _library.lookupFunction<Int32 Function(Pointer<Void>),
            int Function(Pointer<Void>)>('lychee_player_close')
        : null;

/// Null with a core older than API 24 (its options since 26; no release
/// had 24 or 25).
final int Function(
        Pointer<Void>, Pointer<Char>, int, Pointer<LycheeOpenOptions>)?
    playerSwitchSource = _library.providesSymbol('lychee_player_switch_source')
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Pointer<Char>, Int32,
                Pointer<LycheeOpenOptions>),
            int Function(Pointer<Void>, Pointer<Char>, int,
                Pointer<LycheeOpenOptions>)>('lychee_player_switch_source')
        : null;

/// Null with a core older than API 25 (its options since 26).
final int Function(Pointer<Void>, Pointer<Char>, Pointer<LycheeOpenOptions>)?
    playerSetNext = _library.providesSymbol('lychee_player_set_next')
        ? _library.lookupFunction<
            Int32 Function(
                Pointer<Void>, Pointer<Char>, Pointer<LycheeOpenOptions>),
            int Function(Pointer<Void>, Pointer<Char>,
                Pointer<LycheeOpenOptions>)>('lychee_player_set_next')
        : null;

final playerPlay = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_play');

final playerPause = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_pause');

final playerSeek = _library.lookupFunction<Void Function(Pointer<Void>, Int64),
    void Function(Pointer<Void>, int)>('lychee_player_seek');

final playerSetVolume = _library.lookupFunction<
    Void Function(Pointer<Void>, Float),
    void Function(Pointer<Void>, double)>('lychee_player_set_volume');

final nowUs =
    _library.lookupFunction<Int64 Function(), int Function()>('lychee_now_us');

/// Null with a core older than API 20 (the released one a fresh checkout
/// pins), as are [playerSetGain] and [playerGetAudioSamples].
final void Function(Pointer<Void>, double, int)? playerFade =
    _library.providesSymbol('lychee_player_fade')
        ? _library.lookupFunction<Void Function(Pointer<Void>, Float, Int64),
            void Function(Pointer<Void>, double, int)>('lychee_player_fade')
        : null;

final void Function(Pointer<Void>, double, double)? playerSetGain = _library
        .providesSymbol('lychee_player_set_gain')
    ? _library.lookupFunction<Void Function(Pointer<Void>, Float, Float),
        void Function(Pointer<Void>, double, double)>('lychee_player_set_gain')
    : null;

final int Function(Pointer<Void>, int, Pointer<Float>, int)?
    playerGetAudioSamples =
    _library.providesSymbol('lychee_player_get_audio_samples')
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Int64, Pointer<Float>, Int32),
            int Function(Pointer<Void>, int, Pointer<Float>,
                int)>('lychee_player_get_audio_samples')
        : null;

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

/// Null with a core older than API 30.
final int Function(Pointer<Void>, Pointer<Int32>)? playerGetErrorCode =
    _library.providesSymbol('lychee_player_get_error_code')
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Pointer<Int32>),
            int Function(
                Pointer<Void>, Pointer<Int32>)>('lychee_player_get_error_code')
        : null;

final playerGetStats = _library.lookupFunction<
    Void Function(Pointer<Void>, Pointer<LycheeStats>),
    void Function(
        Pointer<Void>, Pointer<LycheeStats>)>('lychee_player_get_stats');

final playerRebuildVideoDecoder = _library.lookupFunction<
    Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_player_rebuild_video_decoder');

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

final playerIsSeekable = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_player_is_seekable');

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

// API 22, absent from older cores (the released one a fresh checkout
// pins): then null, and the player reports no variants and no live window.
final bool _hasVariants =
    _library.providesSymbol('lychee_player_get_variant_count');

final int Function(Pointer<Void>)? playerGetVariantCount = _hasVariants
    ? _library.lookupFunction<Int32 Function(Pointer<Void>),
        int Function(Pointer<Void>)>('lychee_player_get_variant_count')
    : null;

final int Function(Pointer<Void>, int, Pointer<LycheeVariantInfo>)?
    playerGetVariantInfo = _hasVariants
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Int32, Pointer<LycheeVariantInfo>),
            int Function(Pointer<Void>, int,
                Pointer<LycheeVariantInfo>)>('lychee_player_get_variant_info')
        : null;

final int Function(Pointer<Void>, int)? playerSelectVariant = _hasVariants
    ? _library.lookupFunction<Int32 Function(Pointer<Void>, Int32),
        int Function(Pointer<Void>, int)>('lychee_player_select_variant')
    : null;

final void Function(Pointer<Void>, int)? playerSetMaxBitrate = _hasVariants
    ? _library.lookupFunction<Void Function(Pointer<Void>, Int64),
        void Function(Pointer<Void>, int)>('lychee_player_set_max_bitrate')
    : null;

final int Function(Pointer<Void>, Pointer<Int64>, Pointer<Int64>)?
    playerGetSeekableRange = _hasVariants
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Pointer<Int64>, Pointer<Int64>),
            int Function(Pointer<Void>, Pointer<Int64>,
                Pointer<Int64>)>('lychee_player_get_seekable_range')
        : null;

final int Function(Pointer<Void>)? playerGetLiveEdge = _hasVariants
    ? _library.lookupFunction<Int64 Function(Pointer<Void>),
        int Function(Pointer<Void>)>('lychee_player_get_live_edge')
    : null;

/// Null with a core older than API 18 (the released one a fresh checkout
/// pins).
final int Function(Pointer<Void>, Pointer<Char>, Pointer<Char>, Pointer<Char>,
        Pointer<Char>)? playerAddSubtitle =
    _library.providesSymbol('lychee_player_set_subtitle_delay')
        ? _library.lookupFunction<
            Int32 Function(Pointer<Void>, Pointer<Char>, Pointer<Char>,
                Pointer<Char>, Pointer<Char>),
            int Function(Pointer<Void>, Pointer<Char>, Pointer<Char>,
                Pointer<Char>, Pointer<Char>)>('lychee_player_add_subtitle')
        : null;

/// lychee_subtitle_options (API 21).
final class LycheeSubtitleOptions extends Struct {
  @Uint32()
  external int structSize;
  external Pointer<Char> language;
  external Pointer<Char> title;
  external Pointer<Char> encoding;
  external Pointer<Char> headers;
  @Int32()
  external int keepSelection;
}

/// Null with a core older than API 21.
final int Function(Pointer<Void>, Pointer<Char>,
    Pointer<LycheeSubtitleOptions>)? playerAddSubtitleWithOptions = _library
        .providesSymbol('lychee_player_add_subtitle_with_options')
    ? _library.lookupFunction<
            Int32 Function(
                Pointer<Void>, Pointer<Char>, Pointer<LycheeSubtitleOptions>),
            int Function(
                Pointer<Void>, Pointer<Char>, Pointer<LycheeSubtitleOptions>)>(
        'lychee_player_add_subtitle_with_options')
    : null;

/// Null with a core older than API 18.
final void Function(Pointer<Void>, int)? playerSetSubtitleDelay = _library
        .providesSymbol('lychee_player_set_subtitle_delay')
    ? _library.lookupFunction<Void Function(Pointer<Void>, Int64),
        void Function(Pointer<Void>, int)>('lychee_player_set_subtitle_delay')
    : null;

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

final frameRelease = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_frame_release');

final frameGetType = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_frame_get_type');

final frameGetWidth = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_frame_get_width');

final frameGetHeight = _library.lookupFunction<Int32 Function(Pointer<Void>),
    int Function(Pointer<Void>)>('lychee_frame_get_height');

final frameGetData = _library.lookupFunction<
    Pointer<Uint8> Function(Pointer<Void>, Pointer<Int32>),
    Pointer<Uint8> Function(
        Pointer<Void>, Pointer<Int32>)>('lychee_frame_get_data');

/// Null with a core older than API 30.
final Pointer<Void> Function(
        Pointer<Char>, Pointer<LycheeOpenOptions>, int, int)?
    thumbnailerCreate = _library.providesSymbol('lychee_thumbnailer_create')
        ? _library.lookupFunction<
            Pointer<Void> Function(
                Pointer<Char>, Pointer<LycheeOpenOptions>, Int32, Int32),
            Pointer<Void> Function(Pointer<Char>, Pointer<LycheeOpenOptions>,
                int, int)>('lychee_thumbnailer_create')
        : null;

final thumbnailerGet = _library.lookupFunction<
    Pointer<Void> Function(Pointer<Void>, Int64, Int32),
    Pointer<Void> Function(Pointer<Void>, int, int)>('lychee_thumbnailer_get');

final thumbnailerCancel = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_thumbnailer_cancel');

final thumbnailerDestroy = _library.lookupFunction<Void Function(Pointer<Void>),
    void Function(Pointer<Void>)>('lychee_thumbnailer_destroy');

/// lychee_log_listener.
typedef LogListenerNative = Void Function(Pointer<Void>, Int32, Int64);

/// lychee_set_log_level().
final setLogLevel =
    _library.lookupFunction<Void Function(Int32), void Function(int)>(
        'lychee_set_log_level');

/// Null with a core older than API 31.
final void Function(Pointer<NativeFunction<LogListenerNative>>, Pointer<Void>)?
    setLogListener = _library.providesSymbol('lychee_set_log_listener')
        ? _library.lookupFunction<
            Void Function(
                Pointer<NativeFunction<LogListenerNative>>, Pointer<Void>),
            void Function(Pointer<NativeFunction<LogListenerNative>>,
                Pointer<Void>)>('lychee_set_log_listener')
        : null;

/// Null with a core older than API 31.
final void Function(int)? freeLogMessage =
    _library.providesSymbol('lychee_free_log_message')
        ? _library.lookupFunction<Void Function(Int64), void Function(int)>(
            'lychee_free_log_message')
        : null;
