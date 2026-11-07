import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:lychee_player_example/main.dart';

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  SystemChannels.lifecycle.setMessageHandler((_) async => null);
  binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);

  testWidgets('desktop shortcuts and MP3 cover', (tester) async {
    if (!Platform.isMacOS && !Platform.isWindows && !Platform.isLinux) return;
    const media = String.fromEnvironment(
      'LYCHEE_MEDIA',
      defaultValue: '/tmp/lychee-media',
    );
    await tester.pumpWidget(const ExampleApp(url: '$media/artwork.mp3'));
    for (var i = 0; i < 100 && find.byType(Image).evaluate().isEmpty; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      await tester.pump();
    }
    expect(find.byType(Image), findsOneWidget);
    expect(find.byTooltip('Pause'), findsOneWidget);
    await tester.sendKeyEvent(LogicalKeyboardKey.space);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await tester.pump();
    expect(find.byTooltip('Play'), findsOneWidget);
    await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    await tester.pump();
    expect(tester.widget<Slider>(find.byType(Slider)).value, greaterThan(4000));
    await tester.sendKeyEvent(LogicalKeyboardKey.arrowLeft);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    await tester.pump();
    expect(tester.widget<Slider>(find.byType(Slider)).value, lessThan(1500));
    await tester.sendKeyEvent(LogicalKeyboardKey.space);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await tester.pump();
    expect(find.byTooltip('Pause'), findsOneWidget);
    await tester.tap(find.byType(TextField));
    await tester.enterText(find.byType(TextField), 'hello world');
    await tester.sendKeyEvent(LogicalKeyboardKey.space);
    await tester.pump();
    expect(find.byTooltip('Pause'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
