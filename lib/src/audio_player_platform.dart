import 'package:flutter/foundation.dart';

import 'audio_player_common.dart';
import 'ffi_player.dart';

enum DataSourceType { url, file, asset }

abstract class AudioPlayer {
  factory AudioPlayer.file(String path) {
    return AudioPlayer._create(path, DataSourceType.file);
  }

  factory AudioPlayer.url(String url) {
    return AudioPlayer._create(url, DataSourceType.url);
  }

  factory AudioPlayer.asset(String name) {
    return AudioPlayer._create(name, DataSourceType.asset);
  }

  factory AudioPlayer._create(String uri, DataSourceType type) {
    return LycheePlayer(uri, type);
  }

  void seek(Duration duration);

  bool get playWhenReady;

  set playWhenReady(bool value);

  // [0, 1].
  double get volume;

  ///
  /// [volume] [0, 1].
  ///
  set volume(double volume);

  /**
   * Current position has been play.
   *
   * 0 for init state.
   */
  Duration get currentTime;

  /**
   * The duration of media.
   *
   * might be -1 casue can not get duration current.
   */
  Duration get duration;

  bool get isPlaying;

  PlayerStatus get status;

  Listenable get onStateChanged;

  ValueListenable<PlayerStatus> get onStatusChanged;

  bool get hasError;

  ValueListenable<dynamic> get error;

  ValueListenable<List<DurationRange>> get buffered;

  void dispose();
}
