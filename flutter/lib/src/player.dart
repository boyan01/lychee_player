import 'dart:async';
import 'dart:convert';
import 'dart:ffi' hide Size;
import 'dart:math' as math;
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

/// What a track is for, as the container says. Matches lychee_track_role.
enum LycheeTrackRole {
  main(native.kTrackRoleMain),

  /// Spoken commentary (a director's, ...).
  commentary(native.kTrackRoleCommentary),

  /// For viewers who cannot see the picture: audio description, or text
  /// describing the video.
  description(native.kTrackRoleDescription),

  /// For viewers who cannot hear the sound: captions (SDH), or audio with
  /// clearer dialog.
  hearingImpaired(native.kTrackRoleHearingImpaired);

  const LycheeTrackRole(this.value);
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
    this.external = false,
    this.isDefault = false,
    this.isForced = false,
    this.role = LycheeTrackRole.main,
    this.bitrate = 0,
  });

  /// Pass to [LycheePlayer.selectTrack].
  final int index;
  final LycheeTrackType type;

  /// The container's stream index; for a subtitle file, the id
  /// [LycheePlayer.addSubtitle] gave it.
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

  /// A subtitle file [LycheePlayer.addSubtitle] added.
  final bool external;

  /// The container marks it as a track to play by default (Matroska's
  /// FlagDefault, which is set unless a file says otherwise; MP4's enabled
  /// flag; HLS `DEFAULT=YES`).
  final bool isDefault;

  /// Subtitles to show even when subtitles are off, e.g. for dialog in
  /// another language.
  final bool isForced;

  final LycheeTrackRole role;

  /// Bits per second the container declares or its sample sizes imply
  /// (HLS: the variant's `BANDWIDTH`, on its video track); 0 if unknown.
  final int bitrate;
}

/// A variant of an HLS master playlist that this build can play.
@immutable
class LycheeVariant {
  const LycheeVariant({
    required this.index,
    required this.bandwidth,
    this.width = 0,
    this.height = 0,
    this.frameRate = 0,
    this.codecs = '',
    required this.track,
    this.playing = false,
    this.selected = false,
  });

  /// Pass to [LycheePlayer.selectVariant].
  final int index;

  /// Declared bit/s.
  final int bandwidth;

  /// Picture size and frame rate; 0 if the playlist does not say.
  final int width;
  final int height;
  final double frameRate;

  /// The CODECS attribute, or empty.
  final String codecs;

  /// The [LycheeTrack.index] of the video track that plays it.
  final int track;

  /// On screen.
  final bool playing;

  /// [LycheePlayer.selectVariant] keeps to it.
  final bool selected;
}

/// The positions [LycheePlayer.seek] reaches.
@immutable
class LycheeSeekableRange {
  const LycheeSeekableRange(this.start, this.end);

  final Duration start;

  /// Null if unknown.
  final Duration? end;

  @override
  bool operator ==(Object other) =>
      other is LycheeSeekableRange && start == other.start && end == other.end;

  @override
  int get hashCode => Object.hash(start, end);
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
    this.variants = const [],
    this.seekableRange,
    this.liveEdge,
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

  /// Subtitle text on screen, lines separated by '\n'; null if none.
  /// [LycheeVideo] draws it, unless [subtitlePictures] is set (they hold the
  /// text too).
  final String? subtitle;

  /// Bitmap subtitles (PGS, DVB, VobSub): they hold all of the subtitle,
  /// text included. [LycheeVideo]
  /// draws them. Null if none.
  final List<LycheeSubtitlePicture>? subtitlePictures;

  /// The cover picture as the media stores it, usually JPEG or PNG (for
  /// [Image.memory]); null if it has none.
  final Uint8List? artwork;

  /// The variants of an HLS master playlist; empty for other media.
  final List<LycheeVariant> variants;

  /// The positions [LycheePlayer.seek] reaches; null where [seekable] is
  /// false. Live HLS whose playlist keeps a window (an EVENT playlist, or
  /// a sliding one at least six target durations long) can seek within
  /// it: its positions count from the start of the playlist's first
  /// segment when it was opened, so playback starts above zero, at
  /// [liveEdge], and the range moves on in real time.
  final LycheeSeekableRange? seekableRange;

  /// Live HLS that can seek: the position that plays live, as it moves on;
  /// seek there to return to live. Null for other input.
  final Duration? liveEdge;

  // By value, so the player notifies its listeners only when something
  // changed. Lists compare element by element; tracks and pictures are
  // then the same objects unless the player read them again.
  @override
  bool operator ==(Object other) =>
      other is LycheePlayerValue &&
      state == other.state &&
      playing == other.playing &&
      position == other.position &&
      duration == other.duration &&
      buffered == other.buffered &&
      videoSize == other.videoSize &&
      rotation == other.rotation &&
      error == other.error &&
      rate == other.rate &&
      looping == other.looping &&
      seekable == other.seekable &&
      listEquals(tracks, other.tracks) &&
      subtitle == other.subtitle &&
      listEquals(subtitlePictures, other.subtitlePictures) &&
      listEquals(artwork, other.artwork) &&
      listEquals(variants, other.variants) &&
      seekableRange == other.seekableRange &&
      liveEdge == other.liveEdge;

  @override
  int get hashCode => Object.hash(
      state,
      playing,
      position,
      duration,
      buffered,
      videoSize,
      rotation,
      error,
      rate,
      looping,
      seekable,
      Object.hashAll(tracks),
      subtitle,
      subtitlePictures == null ? null : Object.hashAll(subtitlePictures!),
      // Not the bytes: they can be megabytes.
      artwork?.length,
      Object.hashAll(variants),
      seekableRange,
      liveEdge);
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
///
/// Counts are since the open, across track changes.
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
    this.audioDecoder = '',
    this.framesDecoded = 0,
    this.videoBytesRead = 0,
    this.audioBytesRead = 0,
    this.downloadRate,
    this.startupTime = Duration.zero,
    this.rebufferCount = 0,
    this.rebufferTime = Duration.zero,
    this.seekCount = 0,
    this.lastSeekTime = Duration.zero,
    this.videoOffset = Duration.zero,
    this.audioOutputSampleRate = 0,
    this.audioOutputChannels = 0,
    this.audioOutputLatency = Duration.zero,
    this.variantBandwidth,
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

  /// `none` without audio.
  final String audioDecoder;

  /// Frames the video decoder output, including those skipped to reach a
  /// seek target.
  final int framesDecoded;

  /// Bytes read for the selected video and audio tracks.
  final int videoBytesRead;
  final int audioBytesRead;

  /// Bits per second the input last arrived at over the network, null if
  /// unknown.
  final int? downloadRate;

  /// From the open to the first [LycheePlayerState.ready]; zero before.
  final Duration startupTime;

  /// Times playback ran out of data while playing, and the time spent so.
  final int rebufferCount;
  final Duration rebufferTime;

  /// Seeks completed, and how long the last took.
  final int seekCount;
  final Duration lastSeekTime;

  /// Average of how far ahead of the clock (the audio heard) frames are
  /// when shown; negative: behind.
  final Duration videoOffset;

  /// What the audio device plays, 0 without audio, and how far ahead of
  /// now it plays what it takes.
  final int audioOutputSampleRate;
  final int audioOutputChannels;
  final Duration audioOutputLatency;

  /// The bandwidth (bit/s) of the HLS variant on screen, null for other
  /// input.
  final int? variantBandwidth;
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
  List<LycheeVariant> _variants = const [];
  String? _subtitle;
  bool _hasPictures = false;
  int _picturesId = 0;
  List<LycheeSubtitlePicture>? _pictures;
  Uint8List? _artwork;
  // addSubtitle() requests waiting for LYCHEE_EVENT_SUBTITLE_ADDED, by id.
  final Map<int, Completer<bool>> _addingSubtitles = {};

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
  ///
  /// Every http(s) request of the input carries [headers] ('User-Agent',
  /// 'Referer', 'Cookie', any other): the URL, its redirects, and for HLS
  /// every playlist, segment, key, `EXT-X-MAP` and rendition. Throws an
  /// [ArgumentError] if a name or value holds a line break, and an
  /// [UnsupportedError] for headers with a core older than API 21.
  void open(String url, {Map<String, String>? headers}) {
    if (_disposed) {
      return;
    }
    final lines = _headerLines(headers);
    final openWithOptions = native.playerOpenWithOptions;
    if (lines.isNotEmpty && openWithOptions == null) {
      throw UnsupportedError('HTTP headers need lychee API 21');
    }
    _tracks = const [];
    _variants = const [];
    _subtitle = null;
    _hasPictures = false;
    _setPictures(0, null);
    _artwork = null;
    _finishAddingSubtitles();
    final nativeUrl = url.toNativeUtf8();
    if (openWithOptions == null) {
      native.playerOpen(_player, nativeUrl.cast());
    } else {
      final options = calloc<native.LycheeOpenOptions>();
      final nativeHeaders = lines.toNativeUtf8();
      options.ref
        ..structSize = sizeOf<native.LycheeOpenOptions>()
        ..headers = nativeHeaders.cast();
      openWithOptions(_player, nativeUrl.cast(), options);
      malloc.free(nativeHeaders);
      calloc.free(options);
    }
    malloc.free(nativeUrl);
    _refresh();
  }

  /// [headers] as the "Name: value" lines the C API takes.
  static String _headerLines(Map<String, String>? headers) {
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

  void play() {
    if (!_disposed) native.playerPlay(_player);
  }

  void pause() {
    if (!_disposed) native.playerPause(_player);
  }

  /// Recreates video decoding after platform invalidation, keeping playback state.
  void rebuildVideoDecoder() {
    if (!_disposed) native.playerRebuildVideoDecoder(_player);
  }

  /// Ignored where [LycheePlayerValue.seekable] is false.
  void seek(Duration position) {
    if (!_disposed) native.playerSeek(_player, position.inMicroseconds);
  }

  /// 0 - 1. Kept across [open]. Moves there over 10 ms, so the change does
  /// not click, and stops a [fade] under way.
  void setVolume(double volume) {
    if (!_disposed) native.playerSetVolume(_player, volume);
  }

  /// Moves the volume to [to] (0 - 1) over [duration] of audio played, e.g.
  /// to fade a track out before the next one or in after it. Goes on from
  /// where a fade under way has got to, and holds while paused; an [open]
  /// meanwhile starts at [to].
  void fade(double to, Duration duration) {
    if (_disposed) return;
    final fade = native.playerFade;
    if (fade == null) {
      native.playerSetVolume(_player, to);
    } else {
      fade(_player, to, duration.inMicroseconds);
    }
  }

  /// A gain in dB (-60 to +24, 0 by default) on top of the volume, such as
  /// a ReplayGain track or album gain. Kept across [open]: set it for each
  /// input. A boost is lowered so the loudest sample stays within full
  /// scale: [peak], the input's largest sample (ReplayGain's peak, 1.0 at
  /// full scale), if known; otherwise the boost comes down when the
  /// decoder first meets a louder sample, ahead of it playing.
  void setGain(double db, {double peak = 0}) {
    if (!_disposed) native.playerSetGain?.call(_player, db, peak);
  }

  /// The last [frames] samples the audio output played, mixed down to mono,
  /// after volume and gain, at [LycheePlayerStats.audioOutputSampleRate]: for
  /// visualizers. The first call starts recording what plays, so it and
  /// any time the output played nothing (paused) read as silence;
  /// recording stops a second after the last call. Null without an audio
  /// track (or with a core before API 20).
  Float32List? getAudioSamples(int frames) {
    final get = native.playerGetAudioSamples;
    if (_disposed || get == null || frames <= 0) return null;
    final buffer = malloc<Float>(frames);
    try {
      final got = get(_player, native.nowUs(), buffer, frames);
      if (got <= 0) return null;
      return Float32List.fromList(buffer.asTypedList(got));
    } finally {
      malloc.free(buffer);
    }
  }

  /// The magnitude spectrum of what the audio output played last:
  /// [bins] (a power of two) bands from 0 to half the output's sample
  /// rate, each its share of full scale (a full-scale sine at a band's
  /// frequency reads about 1), from a Hann-windowed FFT of the last
  /// 2 × [bins] samples. Null as for [getAudioSamples].
  Float32List? getAudioSpectrum(int bins) {
    final samples = getAudioSamples(2 * bins);
    return samples == null ? null : audioSpectrum(samples);
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
  /// or disables it if [index] is null. Text and bitmap subtitles (PGS,
  /// DVB, VobSub) both switch, and turn off, at once. Returns false if
  /// rejected (wrong type, a subtitle track no decoder takes, or no audio
  /// and no video left); otherwise [LycheePlayerValue.tracks] updates once
  /// the switch is done.
  bool selectTrack(LycheeTrackType type, int? index) {
    if (_disposed) return false;
    return native.playerSelectTrack(_player, type.value, index ?? -1) == 0;
  }

  /// Plays the variant at [index] in [LycheePlayerValue.variants] only, or
  /// adapts to the bandwidth again if [index] is null. Returns false if
  /// rejected (an index out of range, or before the media is loaded);
  /// otherwise [LycheeVariant.playing] moves once it is on screen.
  bool selectVariant(int? index) {
    final select = native.playerSelectVariant;
    if (_disposed || select == null) return false;
    final accepted = select(_player, index ?? -1) == 0;
    _variants = _readVariants();
    _refresh();
    return accepted;
  }

  /// The highest bandwidth (bit/s) automatic variant selection plays, for
  /// slow or metered networks; null for no limit. Kept across [open]. If
  /// every variant is above it, the lowest plays.
  void setMaxBitrate(int? bitrate) {
    if (_disposed) return;
    native.playerSetMaxBitrate
        ?.call(_player, bitrate == null || bitrate < 0 ? 0 : bitrate);
  }

  /// Adds the subtitle file [url] (a path, file:// or http(s):// URL;
  /// SubRip, WebVTT or ASS/SSA) to the media opened last, as a subtitle
  /// track after the container's. Its cue times count from the start of
  /// the media. It is read in the background and, unless [select] is
  /// false, selected once loaded (false keeps the subtitle track selected
  /// before, or none, so several files can be added without the selection
  /// changing); the future says whether it was added (false: it could not
  /// be read or is not a subtitle file, or other media was opened first).
  /// Call it any time after [open]; opening other media drops it.
  ///
  /// Without a byte order mark, the text is in [encoding], an IANA name
  /// such as 'GB18030', 'Big5', 'Shift_JIS' or 'windows-1251'; null reads
  /// UTF-8, or guesses between GB18030 (GBK), Big5 and windows-1252 when
  /// the file is not UTF-8.
  ///
  /// [headers] are HTTP request headers for [url], as in [open] (the
  /// media's are not used); they and `select: false` need a core of API 21
  /// or later, or throw an [UnsupportedError].
  Future<bool> addSubtitle(String url,
      {String? language,
      String? title,
      String? encoding,
      bool select = true,
      Map<String, String>? headers}) {
    final lines = _headerLines(headers);
    final addWithOptions = native.playerAddSubtitleWithOptions;
    if ((lines.isNotEmpty || !select) && addWithOptions == null) {
      throw UnsupportedError('subtitle options need lychee API 21');
    }
    final add = native.playerAddSubtitle;
    if (_disposed || (add == null && addWithOptions == null)) {
      return Future.value(false);
    }
    final strings = [url, language, title, encoding, lines]
        .map((s) => s == null ? nullptr : s.toNativeUtf8())
        .toList();
    final int id;
    if (addWithOptions != null) {
      final options = calloc<native.LycheeSubtitleOptions>();
      options.ref
        ..structSize = sizeOf<native.LycheeSubtitleOptions>()
        ..language = strings[1].cast()
        ..title = strings[2].cast()
        ..encoding = strings[3].cast()
        ..headers = strings[4].cast()
        ..keepSelection = select ? 0 : 1;
      id = addWithOptions(_player, strings[0].cast(), options);
      calloc.free(options);
    } else {
      id = add!(_player, strings[0].cast(), strings[1].cast(),
          strings[2].cast(), strings[3].cast());
    }
    for (final s in strings) {
      if (s != nullptr) malloc.free(s);
    }
    if (id < 0) return Future.value(false);
    // The event may be in flight already: it is handled on this isolate's
    // event loop, after this returns.
    return (_addingSubtitles[id] = Completer<bool>()).future;
  }

  /// Shows the subtitles of every track [delay] later; negative shows them
  /// earlier (a container's only as far as the read is ahead, usually a
  /// few seconds). Kept across [open].
  void setSubtitleDelay(Duration delay) {
    if (_disposed) return;
    native.playerSetSubtitleDelay?.call(_player, delay.inMicroseconds);
  }

  void _finishAddingSubtitles() {
    for (final completer in _addingSubtitles.values) {
      completer.complete(false);
    }
    _addingSubtitles.clear();
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
    String name(Array<Uint8> chars) {
      final codes = <int>[];
      for (var i = 0; i < 32 && chars[i] != 0; i++) {
        codes.add(chars[i]);
      }
      return String.fromCharCodes(codes);
    }

    final s = stats.ref;
    final result = LycheePlayerStats(
      hardwareDecoding: s.hardwareDecoding != 0,
      framesRendered: s.framesRendered,
      framesDropped: s.framesDropped,
      videoDecoder: name(s.videoDecoder),
      videoBuffered: Duration(microseconds: s.videoBufferedUs),
      audioBuffered: Duration(microseconds: s.audioBufferedUs),
      videoDecodeTime: Duration(microseconds: s.videoDecodeTimeUs),
      audioDecoder: name(s.audioDecoder),
      framesDecoded: s.framesDecoded,
      videoBytesRead: s.videoBytesRead,
      audioBytesRead: s.audioBytesRead,
      // A core older than API 16 leaves it 0.
      downloadRate: s.downloadRateBps > 0 ? s.downloadRateBps : null,
      startupTime: Duration(microseconds: s.startupTimeUs),
      rebufferCount: s.rebufferCount,
      rebufferTime: Duration(microseconds: s.rebufferTimeUs),
      seekCount: s.seekCount,
      lastSeekTime: Duration(microseconds: s.lastSeekTimeUs),
      videoOffset: Duration(microseconds: s.videoOffsetUs),
      audioOutputSampleRate: s.audioOutputSampleRate,
      audioOutputChannels: s.audioOutputChannels,
      audioOutputLatency: Duration(microseconds: s.audioOutputLatencyUs),
      variantBandwidth:
          s.variantBandwidthBps > 0 ? s.variantBandwidthBps : null,
    );
    calloc.free(stats);
    return result;
  }

  void _onEvent(int type, int arg0, int arg1) {
    if (_disposed) return;
    if (type == native.kEventMediaInfo || type == native.kEventTracksChanged) {
      _tracks = _readTracks();
    }
    if (type == native.kEventMediaInfo ||
        type == native.kEventTracksChanged ||
        type == native.kEventVariantChanged) {
      _variants = _readVariants();
    }
    if (type == native.kEventMediaInfo) {
      _artwork = _readArtwork();
    } else if (type == native.kEventSubtitleChanged) {
      _subtitle = arg0 > 0 ? _readSubtitle(arg0) : null;
      _hasPictures = arg1 != 0;
      _readPictures();
    } else if (type == native.kEventSubtitleAdded) {
      _addingSubtitles.remove(arg0)?.complete(arg1 != 0);
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
        external: ref.isExternal != 0,
        isDefault: ref.isDefault != 0,
        isForced: ref.forced != 0,
        role: LycheeTrackRole.values.firstWhere((r) => r.value == ref.role,
            orElse: () => LycheeTrackRole.main),
        bitrate: ref.bitrate < 0 ? 0 : ref.bitrate,
      ));
    }
    calloc.free(info);
    return List.unmodifiable(tracks);
  }

  List<LycheeVariant> _readVariants() {
    final getCount = native.playerGetVariantCount;
    final getInfo = native.playerGetVariantInfo;
    if (getCount == null || getInfo == null) return const [];
    final variants = <LycheeVariant>[];
    final info = calloc<native.LycheeVariantInfo>();
    final count = getCount(_player);
    for (var i = 0; i < count; i++) {
      info.ref.structSize = sizeOf<native.LycheeVariantInfo>();
      if (getInfo(_player, i, info) != 0) continue;
      final ref = info.ref;
      variants.add(LycheeVariant(
        index: i,
        bandwidth: ref.bandwidth,
        width: ref.width,
        height: ref.height,
        frameRate: ref.frameRate,
        codecs: _string(ref.codecs, 64),
        track: ref.track,
        playing: ref.playing != 0,
        selected: ref.selected != 0,
      ));
    }
    calloc.free(info);
    return List.unmodifiable(variants);
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
      pending.add(
          completer.future.then((image) => LycheeSubtitlePicture(image, rect)));
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

    LycheeSeekableRange? seekableRange;
    final getRange = native.playerGetSeekableRange;
    if (getRange != null) {
      final start = calloc<Int64>();
      final end = calloc<Int64>();
      if (getRange(_player, start, end) == 0) {
        seekableRange = LycheeSeekableRange(Duration(microseconds: start.value),
            end.value < 0 ? null : Duration(microseconds: end.value));
      }
      calloc.free(start);
      calloc.free(end);
    }
    final edge = native.playerGetLiveEdge?.call(_player) ?? -1;

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
      variants: _variants,
      seekableRange: seekableRange,
      liveEdge: edge < 0 ? null : Duration(microseconds: edge),
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
    _finishAddingSubtitles();
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

/// The magnitude spectrum of [samples] (a power of two long): half as many
/// bands from 0 to half the sample rate, each its share of full scale (a
/// full-scale sine reads about 1 in its band), from a Hann-windowed FFT.
Float32List audioSpectrum(Float32List samples) {
  final n = samples.length;
  if (n < 2 || n & (n - 1) != 0) {
    throw ArgumentError.value(n, 'samples.length', 'not a power of two');
  }
  final re = Float64List(n);
  final im = Float64List(n);
  // Hann window; its mean gain is 1/2.
  for (var i = 0; i < n; i++) {
    re[i] = samples[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / n));
  }
  // Iterative radix-2 FFT: bit-reversed order, then butterflies.
  for (var i = 1, j = 0; i < n; i++) {
    var bit = n >> 1;
    for (; j & bit != 0; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      final t = re[i];
      re[i] = re[j];
      re[j] = t;
    }
  }
  for (var size = 2; size <= n; size <<= 1) {
    final angle = -2 * math.pi / size;
    for (var start = 0; start < n; start += size) {
      for (var k = 0; k < size >> 1; k++) {
        final wr = math.cos(angle * k);
        final wi = math.sin(angle * k);
        final a = start + k;
        final b = a + (size >> 1);
        final xr = re[b] * wr - im[b] * wi;
        final xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
  final bins = Float32List(n >> 1);
  // A sine of amplitude A peaks at A * n / 4 with the window.
  final scale = 4 / n;
  for (var i = 0; i < bins.length; i++) {
    bins[i] = math.sqrt(re[i] * re[i] + im[i] * im[i]) * scale;
  }
  return bins;
}
