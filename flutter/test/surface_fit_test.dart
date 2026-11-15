import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lychee_player/lychee_player.dart';

class _Player extends ValueNotifier<LycheePlayerValue> implements LycheePlayer {
  _Player(Size size) : super(LycheePlayerValue(videoSize: size));

  @override
  int get textureId => 42;

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

void main() {
  testWidgets('native cover keeps the full video scale and center crop',
      (tester) async {
    tester.view.physicalSize = const Size(960, 540);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final calls = <MethodCall>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('lychee_player/surface/42'), (call) async => null);
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('lychee_player'), (call) async {
      calls.add(call);
      return true;
    });
    final player = _Player(const Size(1440, 1080));

    Future<void> check(Size size, BoxFit fit, List<int> expected,
        {int rotation = 0, bool surfaceView = true}) async {
      calls.clear();
      player.value = LycheePlayerValue(videoSize: size, rotation: rotation);
      await tester.pumpWidget(Directionality(
        textDirection: TextDirection.ltr,
        child: LycheeVideo(player, surfaceView: surfaceView, fit: fit),
      ));
      await tester.pumpAndSettle();
      final call = calls.lastWhere((call) => call.method == 'setSurfaceView');
      expect(call.arguments['rect'], expected,
          reason: '$size $fit rot=$rotation');
      expect(call.arguments['clipRect'], [0, 0, 960, 540],
          reason: '$size $fit rot=$rotation');
      expect(find.byType(Texture), findsNothing);
    }

    await check(const Size(1440, 1080), BoxFit.cover, [0, -90, 960, 720]);
    await check(const Size(2560, 1080), BoxFit.cover, [-160, 0, 1280, 540]);
    await check(const Size(1440, 1080), BoxFit.contain, [120, 0, 720, 540]);
    await check(const Size(1440, 1080), BoxFit.fill, [0, 0, 960, 540]);
    await check(const Size(1440, 1080), BoxFit.contain, [120, 0, 720, 540],
        surfaceView: false);
    await check(const Size(1440, 1080), BoxFit.contain, [278, 0, 405, 540],
        rotation: 90);

    String? surfaceError;
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: SingleChildScrollView(
        child: LycheeVideo(player, onSurfaceError: (e) => surfaceError = e),
      ),
    ));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.byType(Texture), findsNothing);
    expect(surfaceError, contains('SurfaceView'));

    await tester.pumpWidget(const SizedBox());
    player.dispose();
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('lychee_player/surface/42'), null);
    tester.binding.defaultBinaryMessenger
        .setMockMethodCallHandler(const MethodChannel('lychee_player'), null);
  }, variant: TargetPlatformVariant.only(TargetPlatform.android));
}
