import 'dart:html' as html;

import 'package:flutter/foundation.dart';

import 'audio_player_common.dart';
import 'audio_player_platform.dart' as api;

class AudioPlayer implements api.AudioPlayer {
  final String _url;

  final html.AudioElement _audioElement;

  final ValueNotifier<bool> _playWhenReady = ValueNotifier(false);

  bool _isPlaying = false;

  Duration _duration = const Duration(microseconds: -1);

  final ValueNotifier<PlayerStatus> _status = ValueNotifier(PlayerStatus.Idle);

  final ValueNotifier<List<DurationRange>> _buffered = ValueNotifier(const []);

  final ValueNotifier<dynamic> _onError = ValueNotifier(null);

  Listenable? _onStateChanged;

  AudioPlayer._create(this._url) : _audioElement = html.AudioElement(_url) {
    _audioElement.onPlaying.forEach((element) {
      debugPrint("on playing: $element");
      _isPlaying = true;
      if (!_playWhenReady.value) {
        _playWhenReady.value = true;
      }
    });
    _audioElement.onPause.forEach((element) {
      debugPrint("on pause: $element");
      _isPlaying = false;
      if (_playWhenReady.value) {
        _playWhenReady.value = false;
      }
    });
    _audioElement.onError.forEach((element) {
      _isPlaying = false;
      _onError.value = _audioElement.error;
      _status.value = PlayerStatus.Idle;
    });
    _audioElement.onWaiting.forEach((element) {
      _isPlaying = false;
      _status.value = PlayerStatus.Buffering;
    });
    _audioElement.onCanPlayThrough.forEach((element) {
      _status.value = PlayerStatus.Ready;
    });
    _audioElement.onEnded.forEach((element) {
      _isPlaying = false;
      _status.value = PlayerStatus.End;
    });
    _audioElement.onDurationChange.forEach((element) {
      final duration = _audioElement.duration * 1000;
      _duration = Duration(milliseconds: duration.toInt());
    });

    _audioElement.addEventListener("progress", (event) {
      _updateBuffered();
    });
    _audioElement.onLoadedData.forEach((element) {
      _updateBuffered();
    });
    _audioElement.onLoad.forEach((element) {
      _updateBuffered();
    });
    _status.addListener(() {
      if (status == PlayerStatus.Ready && _playWhenReady.value) {
        _audioElement.play().catchError((e, s) {
          _playWhenReady.value = false;
          debugPrint(e?.toString());
          debugPrint(s?.toString());
        });
      }
    });
  }

  factory AudioPlayer.file(String path) {
    throw UnsupportedError("web player can not play file.");
  }

  factory AudioPlayer.url(String url) {
    return AudioPlayer._create(url);
  }

  factory AudioPlayer.asset(String name) {
    final url =
        "${html.window.location.href.replaceAll("/#/", "")}/assets/${name}";
    return AudioPlayer._create(url);
  }

// TODO
  @override
  int volume = 0;

  void _updateBuffered() {
    final buffered = _audioElement.buffered;
    final ranges = <DurationRange>[];
    for (var i = 0; i < buffered.length; i++) {
      final start = buffered.start(i) * 1000;
      final end = buffered.end(i) * 1000;
      ranges.add(DurationRange.mills(start.toInt(), end.toInt()));
    }
    debugPrint("_updateBuffered ${ranges}");
    _buffered.value = ranges;
  }

  @override
  bool get playWhenReady {
    return _playWhenReady.value;
  }

  @override
  set playWhenReady(bool value) {
    if (_playWhenReady.value == value) {
      return;
    }
    _playWhenReady.value = value;
    if (value && _status.value == PlayerStatus.Ready) {
      _audioElement.play().catchError((e, s) {
        _playWhenReady.value = false;
        debugPrint(e?.toString());
        debugPrint(s?.toString());
      });
    }
    if (!value && _isPlaying) {
      _audioElement.pause();
    }
  }

  @override
  ValueListenable<List<DurationRange>> get buffered => _buffered;

  @override
  Duration get currentTime {
    final played = _audioElement.currentTime;
    return Duration(milliseconds: (played * 1000).toInt());
  }

  @override
  void dispose() {
    _audioElement.pause();
    _audioElement.src = "";
    _audioElement.load();
    _audioElement.remove();
  }

  @override
  Duration get duration => _duration;

  @override
  ValueListenable get error => _onError;

  @override
  bool get hasError => error.value != null;

  @override
  bool get isPlaying => _playWhenReady.value && status == PlayerStatus.Ready;

  @override
  Listenable get onStateChanged {
    if (_onStateChanged == null) {
      _onStateChanged = Listenable.merge([
        _status,
        _playWhenReady,
      ]);
    }
    return _onStateChanged!;
  }

  @override
  ValueListenable<PlayerStatus> get onStatusChanged => _status;

  @override
  void seek(Duration duration) {
    _audioElement.currentTime = duration.inMilliseconds / 1000.0;
  }

  @override
  PlayerStatus get status => _status.value;
}
