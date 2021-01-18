import 'dart:io';

import 'audio_player_channel.dart';
import 'audio_player_ffi.dart';
import 'audio_player_platform.dart' as api;

abstract class AudioPlayer implements api.AudioPlayer {
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
    if (Platform.isLinux) {
      return FfiAudioPlayer(uri, type);
    }
    return ChannelAudioPlayer.create(uri, type);
  }
}

enum DataSourceType { url, file, asset }
