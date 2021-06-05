// You have generated a new plugin project without
// specifying the `--platforms` flag. A plugin project supports no platforms is generated.
// To add platforms, run `flutter create -t plugin --platforms <platforms> .` under the same
// directory. You can also find a detailed instruction on how to add platforms in the `pubspec.yaml` at https://flutter.dev/docs/development/packages-and-plugins/developing-packages#plugin-platforms.

export 'audio_player_common.dart';
export 'audio_player_platform.dart'
    if (dart.library.io) 'audio_player_io.dart'
    if (dart.library.html) 'audio_player_web.dart';
export 'video_view.dart';
