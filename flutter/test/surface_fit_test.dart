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
  testWidgets('Android uses Flutter texture by default and SurfaceView on request',
      (tester) async {
    tester.view.physicalSize = const Size(960, 540);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final calls = <MethodCall>[];
    var surfaceSupported = true;
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('lychee_player/surface/42'), (call) async => null);
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('lychee_player'), (call) async {
      calls.add(call);
      if (call.method == 'setSurfaceView') return surfaceSupported;
      return true;
    });
    final player = _Player(const Size(1440, 1080));

    // Default surfaceView: false renders a Flutter Texture without setSurfaceView.
    for (final rotation in [0, 90]) {
      calls.clear();
      player.value = LycheePlayerValue(
          videoSize: const Size(1440, 1080), rotation: rotation);
      await tester.pumpWidget(Directionality(
        textDirection: TextDirection.ltr,
        child: LycheeVideo(player, fit: BoxFit.cover),
      ));
      await tester.pumpAndSettle();
      expect(find.byType(Texture), findsOneWidget, reason: 'rot=$rotation');
      expect(calls.where((c) => c.method == 'setSurfaceView'), isEmpty);
    }

    Future<void> check(Size size, BoxFit fit, List<int> expected,
        {int rotation = 0}) async {
      calls.clear();
      player.value = LycheePlayerValue(videoSize: size, rotation: rotation);
      await tester.pumpWidget(Directionality(
        textDirection: TextDirection.ltr,
        child: LycheeVideo(player, surfaceView: true, fit: fit),
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
    await check(const Size(1440, 1080), BoxFit.contain, [278, 0, 405, 540],
        rotation: 90);

    // Without onSurfaceError, surfaceView: true falls back to Texture on
    // unbounded constraints, non-quarter-turn rotation, or attach refusal.
    calls.clear();
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: SingleChildScrollView(
        child: LycheeVideo(player, surfaceView: true),
      ),
    ));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.byType(Texture), findsOneWidget);

    player.value = const LycheePlayerValue(
        videoSize: Size(1440, 1080), rotation: 45);
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: LycheeVideo(player, surfaceView: true),
    ));
    await tester.pumpAndSettle();
    expect(find.byType(Texture), findsOneWidget);

    surfaceSupported = false;
    player.value = const LycheePlayerValue(videoSize: Size(1440, 1080));
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: LycheeVideo(player, surfaceView: true),
    ));
    await tester.pumpAndSettle();
    expect(find.byType(Texture), findsOneWidget);

    // With onSurfaceError, errors are reported without Texture fallback.
    String? surfaceError;
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: SingleChildScrollView(
        child: LycheeVideo(player,
            surfaceView: true, onSurfaceError: (e) => surfaceError = e),
      ),
    ));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.byType(Texture), findsNothing);
    expect(surfaceError, contains('SurfaceView'));

    surfaceError = null;
    await tester.pumpWidget(Directionality(
      textDirection: TextDirection.ltr,
      child: LycheeVideo(player,
          surfaceView: true, onSurfaceError: (e) => surfaceError = e),
    ));
    await tester.pumpAndSettle();
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
