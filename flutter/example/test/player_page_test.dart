import 'dart:io';

import 'package:desktop_drop/desktop_drop.dart';
// desktop_drop exposes its FFI transport test seam from this library.
// ignore: implementation_imports
import 'package:desktop_drop/src/native_transport.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lychee_player_example/main.dart';

void main() {
  setUpAll(() {
    NativeTransport.debugOverride = NativeTransport.testing()
      ..invokeHandler = (call) async => {'view': 0, 'generation': 1};
  });
  testWidgets('empty player fits narrow and short windows', (tester) async {
    addTearDown(() => tester.view.resetPhysicalSize());
    addTearDown(() => tester.view.resetDevicePixelRatio());
    tester.view.devicePixelRatio = 1;
    for (final size in [const Size(320, 568), const Size(1000, 360)]) {
      tester.view.physicalSize = size;
      await tester.pumpWidget(const ExampleApp());
      await tester.pump();
      expect(find.text('Drop a file to play'), findsOneWidget);
      expect(tester.takeException(), isNull);
    }
    // An empty source must not create a native player.
    await tester.tap(find.text('Open'));
    await tester.pump();
    expect(tester.takeException(), isNull);
  });

  testWidgets('drag highlight clears on leave and only files are accepted', (
    tester,
  ) async {
    await tester.pumpWidget(const ExampleApp());
    final target = tester.widget<DropTarget>(find.byType(DropTarget));
    expect(
      target.enabled,
      Platform.isMacOS || Platform.isLinux || Platform.isWindows,
    );
    target.onEnter!(
      DragDetails(
        items: const [],
        localPosition: Offset.zero,
        globalPosition: Offset.zero,
      ),
    );
    await tester.pump();
    expect(find.text('Release to play'), findsOneWidget);
    target.onLeave!();
    await tester.pump();
    expect(find.text('Release to play'), findsNothing);

    expect(target.acceptedKinds, {DropItemKind.file});
    await target.onDrop(
      DropDetails(
        items: const [],
        localPosition: Offset.zero,
        globalPosition: Offset.zero,
        operation: DropOperation.copy,
      ),
    );
    await tester.pump();
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      isEmpty,
    );
    expect(tester.takeException(), isNull);
  });
}
