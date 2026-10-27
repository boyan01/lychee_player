import 'dart:async';
import 'dart:convert';
import 'dart:ffi' hide Size;
import 'dart:ui' as ui;

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'bindings.dart' as native;

/// Matches lychee_state.
enum LycheePlayerState { idle, loading, ready, buffering, ended, error }

/// Matches lychee_track_type.
enum LycheeTrackType {
  video(native.kTrackVideo),
  audio(native.kTrackAudio),
  subtitle(native.kTrackSubtitle);

  const LycheeTrackType(this.value);
  final int value;
}

/// An audio, video or subtitle stream of the open media.
@immutable
class LycheeTrack {
  const LycheeTrack({
    required this.index,
    required this.type,
    required this.id,
    required this.selected,
    this.codec = '',
    this.language = '',
    this.title = '',
    this.width = 0,
    this.height = 0,
    this.sampleRate = 0,
    this.channels = 0,
  });

  /// Pass to [LycheePlayer.selectTrack].
  final int index;
  final LycheeTrackType type;

  /// The container's stream index.
  final int id;
  final bool selected;
  final String codec;
  final String language;
  final String title;

  /// Video only: display size.
  final int width;
  final int height;

  /// Audio only.
  final int sampleRate;
  final int channels;
}

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
    this.rate = 1.0,
    this.looping = false,
    this.seekable = false,
    this.tracks = const [],
    this.subtitle,
    this.subtitlePictures,
    this.artwork,
  });

  final LycheePlayerState state;

  /// Whether playback is requested; it actually advances only when [state]
  /// is [LycheePlayerState.ready]. Stays true once ended, where [play]
  /// starts from the beginning.
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

  /// Playback speed; audio keeps its pitch.
  final double rate;
  final bool looping;

  /// Whether [LycheePlayer.seek] can move playback; false until the media
  /// is loaded, for live input and on error. An http server without range
  /// requests cannot seek: seeks there are ignored.
  final bool seekable;
  final List<LycheeTrack> tracks;

  /// Subtitle text on screen, lines separated by '\n'; null if none. Draw it
  /// over the video yourself, unless [subtitlePictures] is set.
  final String? subtitle;

  /// Bitmap subtitles (PGS, DVB, VobSub): they hold all of the subtitle,
  /// text included. [LycheeVideo]
  /// draws them. Null if none.
  final List<LycheeSubtitlePicture>? subtitlePictures;

  /// The cover picture as the media stores it, usually JPEG or PNG (for
  /// [Image.memory]); null if it has none.
  final Uint8List? artwork;
}

/// A subtitle picture and where it goes on the video, in pixels of its
/// display size after rotation.
@immutable
class LycheeSubtitlePicture {
  const LycheeSubtitlePicture(this.image, this.rect);

  final ui.Image image;
  final Rect rect;
}

/// Playback statistics, read on demand with [LycheePlayer.getStats].
@immutable
class LycheePlayerStats {
  const LycheePlayerStats({
    this.hardwareDecoding = false,
    this.framesRendered = 0,
    this.framesDropped = 0,
    this.videoDecoder = '',
    this.videoBuffered = Duration.zero,
    this.audioBuffered = Duration.zero,
    this.videoDecodeTime = Duration.zero,
  });

  final bool hardwareDecoding;

  /// Frames handed to the display.
  final int framesRendered;

  /// Decoded frames skipped because a later one was due.
  final int framesDropped;
  final String videoDecoder;

  /// Media read ahead of the playback position.
  final Duration videoBuffered;
  final Duration audioBuffered;

  /// Average time decoding one video packet takes.
  final Duration videoDecodeTime;
}

class LycheePlayer extends ValueNotifier<LycheePlayerValue>
    implements Finalizable {
  LycheePlayer._(this._player, this._events, this.textureId)
      : super(const LycheePlayerValue()) {
    _clearEvents.attach(this, _player, detach: this);
  }

  static const _channel = MethodChannel('lychee_player');

  // A hot restart ends this isolate without dispose(), and the player keeps
  // running until the next isolate's first create(). Before the isolate
  // deletes its NativeCallables, it runs this, which returns once the
  // player is no longer in the callback.
  static final _clearEvents =
      NativeFinalizer(native.playerClearEventCallbackPointer);

  final Pointer<Void> _player;
  final NativeCallable<native.EventCallbackNative> _events;
  final int textureId;
  Timer? _ticker;
  bool _disposed = false;
  List<LycheeTrack> _tracks = const [];
  String? _subtitle;
  bool _hasPictures = false;
  int _picturesId = 0;
  List<LycheeSubtitlePicture>? _pictures;
  Uint8List? _artwork;

  // A hot restart drops the previous isolate's players without dispose();
  // the plugin destroys them before this isolate creates its first.
  static Future<void>? _orphansDestroyed;

  static Future<LycheePlayer> create({bool hardwareDecoding = true}) async {
    await (_orphansDestroyed ??= _channel.invokeMethod<void>('disposeAll'));
    final config = calloc<native.LycheeConfig>();
    native.configInit(config, sizeOf<native.LycheeConfig>());
    config.ref.videoFrameTypes = native.platformFrameType;
    config.ref.hardwareDecoding = hardwareDecoding ? 1 : 0;
    final player = native.playerCreate(config);
    calloc.free(config);

    int? textureId;
    try {
      textureId = await _channel
          .invokeMethod<int>('createTexture', {'player': player.address});
    } finally {
      // No player to return, so nobody else can dispose of it.
      if (textureId == null) {
        native.playerDestroy(player);
      }
    }
    // The callback only once the finalizer that clears it is attached:
    // nothing is open yet, so no event is missed.
    late final LycheePlayer instance;
    final events = NativeCallable<native.EventCallbackNative>.listener(
        (Pointer<Void> _, int type, int arg0, int arg1) =>
            instance._onEvent(type, arg0, arg1));
    instance = LycheePlayer._(player, events, textureId!);
    native.playerSetEventCallback(player, events.nativeFunction, nullptr);
    instance._ticker = Timer.periodic(
        const Duration(milliseconds: 200), (_) => instance._onTick());
    return instance;
  }

  // Calls after dispose() are ignored: the native player may already be
  // destroyed, and widgets can still hold this object until their next build.

  /// Opens [url]. Calling it again replaces the current media on this
  /// player; volume, rate, looping and play/pause are kept.
  void open(String url) {
    if (_disposed) {
      return;
    }
    _tracks = const [];
    _subtitle = null;
    _hasPictures = false;
    _setPictures(0, null);
    _artwork = null;
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

  /// Ignored where [LycheePlayerValue.seekable] is false.
  void seek(Duration position) {
    if (!_disposed) native.playerSeek(_player, position.inMicroseconds);
  }

  void setVolume(double volume) {
    if (!_disposed) native.playerSetVolume(_player, volume);
  }

  /// Playback speed, clamped to 0.25 - 4.0. Audio keeps its pitch.
  void setRate(double rate) {
    if (_disposed) return;
    native.playerSetRate(_player, rate);
    _refresh();
  }

  /// Whether to play again from the start instead of ending.
  void setLooping(bool looping) {
    if (_disposed) return;
    native.playerSetLooping(_player, looping ? 1 : 0);
    _refresh();
  }

  /// Switches [type] to the track at [index] in [LycheePlayerValue.tracks],
  /// or disables it if [index] is null. Returns false if rejected (wrong
  /// type, bitmap subtitles, or no audio and no video left); otherwise
  /// [LycheePlayerValue.tracks] updates once the switch is done.
  bool selectTrack(LycheeTrackType type, int? index) {
    if (_disposed) return false;
    return native.playerSelectTrack(_player, type.value, index ?? -1) == 0;
  }

  /// A container tag such as 'title', 'artist' or 'album'; null if absent.
  String? getMetadata(String key) {
    if (_disposed) return null;
    final nativeKey = key.toNativeUtf8();
    try {
      final length =
          native.playerGetMetadata(_player, nativeKey.cast(), nullptr, 0);
      if (length < 0) return null;
      final buffer = calloc<Uint8>(length + 1);
      native.playerGetMetadata(
          _player, nativeKey.cast(), buffer.cast(), length + 1);
      final value =
          utf8.decode(buffer.asTypedList(length), allowMalformed: true);
      calloc.free(buffer);
      return value;
    } finally {
      malloc.free(nativeKey);
    }
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
      videoBuffered: Duration(microseconds: stats.ref.videoBufferedUs),
      audioBuffered: Duration(microseconds: stats.ref.audioBufferedUs),
      videoDecodeTime: Duration(microseconds: stats.ref.videoDecodeTimeUs),
    );
    calloc.free(stats);
    return result;
  }

  void _onEvent(int type, int arg0, int arg1) {
    if (_disposed) return;
    if (type == native.kEventMediaInfo || type == native.kEventTracksChanged) {
      _tracks = _readTracks();
    }
    if (type == native.kEventMediaInfo) {
      _artwork = _readArtwork();
    } else if (type == native.kEventSubtitleChanged) {
      _subtitle = arg0 > 0 ? _readSubtitle(arg0) : null;
      _hasPictures = arg1 != 0;
      _readPictures();
    }
    _refresh();
  }

  Uint8List? _readArtwork() {
    final size = native.playerGetArtwork(_player, nullptr, 0);
    if (size < 0) return null;
    final buffer = calloc<Uint8>(size);
    final copied = native.playerGetArtwork(_player, buffer, size);
    final artwork =
        Uint8List.fromList(buffer.asTypedList(copied.clamp(0, size)));
    calloc.free(buffer);
    return artwork;
  }

  List<LycheeTrack> _readTracks() {
    final tracks = <LycheeTrack>[];
    final info = calloc<native.LycheeTrackInfo>();
    final count = native.playerGetTrackCount(_player);
    for (var i = 0; i < count; i++) {
      info.ref.structSize = sizeOf<native.LycheeTrackInfo>();
      if (native.playerGetTrackInfo(_player, i, info) != 0) continue;
      final ref = info.ref;
      tracks.add(LycheeTrack(
        index: i,
        type: LycheeTrackType.values.firstWhere((t) => t.value == ref.type),
        id: ref.id,
        selected: ref.selected != 0,
        codec: _string(ref.codec, 32),
        language: _string(ref.language, 16),
        title: _string(ref.title, 128),
        width: ref.width,
        height: ref.height,
        sampleRate: ref.sampleRate,
        channels: ref.channels,
      ));
    }
    calloc.free(info);
    return List.unmodifiable(tracks);
  }

  String? _readSubtitle(int length) {
    // The text may have changed since the event; read what is there now.
    final size = length + 256;
    final buffer = calloc<Uint8>(size);
    final actual = native.playerGetSubtitle(_player, 0, buffer.cast(), size);
    final text = actual <= 0
        ? null
        : utf8.decode(buffer.asTypedList(actual < size ? actual : size - 1),
            allowMalformed: true);
    calloc.free(buffer);
    return text;
  }

  // Pictures can change without an event (ASS animations), so this runs on
  // every tick too; it costs nothing while the id stays the same.
  void _readPictures() {
    final videoSize = value.videoSize;
    if (!_hasPictures || videoSize == null) {
      _setPictures(0, null);
      return;
    }
    final turns = native.playerGetVideoRotation(_player) ~/ 90;
    final size = turns.isOdd ? videoSize.flipped : videoSize;
    final subtitle = native.playerAcquireSubtitle(
        _player, 0, size.width.round(), size.height.round());
    if (subtitle == nullptr) {
      _setPictures(0, null);
      return;
    }
    final id = native.subtitleGetId(subtitle);
    if (id == _picturesId) {
      native.subtitleRelease(subtitle);
      return;
    }
    _picturesId = id;
    final pending = <Future<LycheeSubtitlePicture>>[];
    final info = calloc<native.LycheeSubtitleImage>();
    final count = native.subtitleGetImageCount(subtitle);
    for (var i = 0; i < count; i++) {
      info.ref.structSize = sizeOf<native.LycheeSubtitleImage>();
      if (native.subtitleGetImage(subtitle, i, info) != 0) continue;
      final image = info.ref;
      final rect = Rect.fromLTWH(image.x.toDouble(), image.y.toDouble(),
          image.width.toDouble(), image.height.toDouble());
      // Copied out: the native pixels go with the subtitle.
      final pixels = Uint8List.fromList(
          image.data.asTypedList(image.stride * image.bitmapHeight));
      final completer = Completer<ui.Image>();
      ui.decodeImageFromPixels(pixels, image.bitmapWidth, image.bitmapHeight,
          ui.PixelFormat.rgba8888, completer.complete,
          rowBytes: image.stride);
      pending.add(completer.future
          .then((image) => LycheeSubtitlePicture(image, rect)));
    }
    calloc.free(info);
    native.subtitleRelease(subtitle);
    Future.wait(pending).then((pictures) {
      if (_disposed || id != _picturesId) {
        for (final picture in pictures) {
          picture.image.dispose();
        }
        return;
      }
      _setPictures(id, pictures);
      _refresh();
    });
  }

  void _setPictures(int id, List<LycheeSubtitlePicture>? pictures) {
    _picturesId = id;
    for (final picture in _pictures ?? const <LycheeSubtitlePicture>[]) {
      picture.image.dispose();
    }
    _pictures = pictures;
  }

  static String _string(Array<Uint8> array, int size) {
    final bytes = <int>[];
    for (var i = 0; i < size && array[i] != 0; i++) {
      bytes.add(array[i]);
    }
    return utf8.decode(bytes, allowMalformed: true);
  }

  void _onTick() {
    final state = value.state;
    if (value.playing ||
        state == LycheePlayerState.loading ||
        state == LycheePlayerState.buffering) {
      _refresh();
    }
    if (_hasPictures) {
      _readPictures();
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
      rate: native.playerGetRate(_player),
      looping: native.playerIsLooping(_player) != 0,
      seekable: native.playerIsSeekable(_player) != 0,
      tracks: _tracks,
      subtitle: _subtitle,
      subtitlePictures: _pictures,
      artwork: _artwork,
    );
  }

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _ticker?.cancel();
    _setPictures(0, null);
    super.dispose();
    _release();
  }

  Future<void> _release() async {
    // The texture must stop pulling frames before the player goes away, and
    // the player must stop calling back before the callable is closed.
    await _channel.invokeMethod('disposeTexture', {'textureId': textureId});
    _clearEvents.detach(this);
    native.playerDestroy(_player);
    _events.close();
  }
}
