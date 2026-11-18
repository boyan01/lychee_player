import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart' show AppLifecycleState;
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:lychee_player/lychee_player.dart';

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  SystemChannels.lifecycle.setMessageHandler((_) async => null);
  binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);

  test('rejected open keeps artwork, tracks and pending subtitles', () async {
    const media = String.fromEnvironment(
      'LYCHEE_MEDIA',
      defaultValue: '/tmp/lychee-media',
    );
    final player = await LycheePlayer.create();
    addTearDown(player.dispose);
    player.setVolume(0);
    player.open('$media/artwork.mp3');
    player.play();

    for (var i = 0; i < 100 && player.value.artwork == null; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    final current = player.value;
    expect(current.tracks, isNotEmpty);
    expect(current.artwork, isNotEmpty);
    expect(current.playing, isTrue);

    // The native result is delivered to Dart after this synchronous open,
    // even if loading the sidecar finishes on its own thread immediately.
    final subtitle = player.addSubtitle('$media/subs.srt');
    player.open('$media/h264_aac.mp4', audioUrls: ['']);
    expect(player.value.tracks, current.tracks);
    expect(player.value.artwork, current.artwork);
    expect(player.value.playing, isTrue);
    expect(await subtitle.timeout(const Duration(seconds: 10)), isTrue);
  });
}
