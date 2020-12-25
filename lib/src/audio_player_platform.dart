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

  Duration get currentTime;

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
