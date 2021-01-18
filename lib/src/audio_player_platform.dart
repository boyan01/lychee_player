import 'package:flutter/foundation.dart';

import 'audio_player_common.dart';

abstract class AudioPlayer {
  factory AudioPlayer.file(String path) {
    throw UnimplementedError();
  }

  factory AudioPlayer.url(String url) {
    throw UnimplementedError();
  }

  factory AudioPlayer.asset(String name) {
    throw UnimplementedError();
  }

  void seek(Duration duration);

  bool get playWhenReady;

  set playWhenReady(bool value);

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
