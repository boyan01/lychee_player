import 'dart:async';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter_test/flutter_test.dart';
import 'package:lychee_player/src/thumbnailer.dart';

ThumbnailPicture _picture({int width = 1}) => (
      pixels: Uint8List.fromList([1, 2, 3, 255]),
      width: width,
      height: 1,
      stride: 4,
    );

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'a worker error settles its request and the newest request runs',
    () async {
      final worker = Completer<ThumbnailPicture?>();
      final requests = <(int, bool)>[];
      var destroyed = 0;
      final thumbnailer = createThumbnailerForTesting(
        load: (micros, exact) {
          requests.add((micros, exact));
          return micros == 0 ? worker.future : Future.value(_picture());
        },
        cancel: () {},
        destroy: () => destroyed++,
      );
      addTearDown(thumbnailer.dispose);

      final error = StateError('worker failed');
      final failed = expectLater(
        thumbnailer.thumbnailAt(Duration.zero),
        throwsA(same(error)),
      );
      final superseded = thumbnailer.thumbnailAt(const Duration(seconds: 1));
      final next = thumbnailer.thumbnailAt(
        const Duration(seconds: 2),
        exact: true,
      );
      expect(await superseded, isNull);
      worker.completeError(error);
      await failed;
      final image = await next;
      addTearDown(() => image?.dispose());
      expect(image?.width, 1);
      expect(requests, [(0, false), (2000000, true)]);
      thumbnailer.dispose();
      expect(destroyed, 1);
    },
  );

  test('a codec error settles its request and the queue recovers', () async {
    final thumbnailer = createThumbnailerForTesting(
      load: (micros, _) async => _picture(width: micros == 0 ? 0 : 1),
      cancel: () {},
      destroy: () {},
    );
    addTearDown(thumbnailer.dispose);

    final failed = expectLater(
      thumbnailer.thumbnailAt(Duration.zero),
      throwsA(isA<Exception>()),
    );
    final next = thumbnailer.thumbnailAt(const Duration(seconds: 1));
    await failed;
    final image = await next;
    addTearDown(() => image?.dispose());
    expect(image?.width, 1);
  });

  test(
    'disposing during image creation releases the image and the handle',
    () async {
      var cancelled = 0;
      var destroyed = 0;
      ui.Image? created;
      final released = <ui.Image>[];
      final thumbnailer = createThumbnailerForTesting(
        load: (micros, exact) async => _picture(),
        cancel: () => cancelled++,
        destroy: () => destroyed++,
      );
      addTearDown(thumbnailer.dispose);
      final onCreate = ui.Image.onCreate;
      final onDispose = ui.Image.onDispose;
      addTearDown(() {
        ui.Image.onCreate = onCreate;
        ui.Image.onDispose = onDispose;
      });
      ui.Image.onCreate = (image) {
        onCreate?.call(image);
        created = image;
        thumbnailer.dispose();
      };
      ui.Image.onDispose = (image) {
        onDispose?.call(image);
        released.add(image);
      };

      final running = thumbnailer.thumbnailAt(Duration.zero);
      final queued = thumbnailer.thumbnailAt(const Duration(seconds: 1));
      final result = await running;
      addTearDown(() => result?.dispose());
      expect(result, isNull);
      expect(await queued, isNull);
      expect(created, isNotNull);
      expect(released, [same(created)]);
      expect(cancelled, greaterThanOrEqualTo(1));
      expect(destroyed, 1);
      thumbnailer.dispose();
      expect(await thumbnailer.thumbnailAt(Duration.zero), isNull);
      expect(destroyed, 1);
    },
  );

  test('disposing during a worker error still settles and destroys', () async {
    final worker = Completer<ThumbnailPicture?>();
    var cancelled = 0;
    var destroyed = 0;
    final thumbnailer = createThumbnailerForTesting(
      load: (micros, exact) => worker.future,
      cancel: () => cancelled++,
      destroy: () => destroyed++,
    );
    addTearDown(thumbnailer.dispose);

    final running = thumbnailer.thumbnailAt(Duration.zero);
    thumbnailer.dispose();
    expect(cancelled, 1);
    expect(destroyed, 0);
    worker.completeError(StateError('worker failed'));
    expect(await running, isNull);
    expect(destroyed, 1);
  });

  test('disposing before FFI cancels a late worker and destroys once',
      () async {
    final enterFfi = Completer<void>();
    final blockedOpen = Completer<ThumbnailPicture?>();
    var inFfi = false;
    var cancelled = 0;
    var destroyed = 0;
    final thumbnailer = createThumbnailerForTesting(
      load: (micros, exact) async {
        await enterFfi.future;
        inFfi = true;
        return blockedOpen.future;
      },
      cancel: () {
        cancelled++;
        // Like native cancel, this cannot interrupt a future Get() call.
        if (inFfi && !blockedOpen.isCompleted) blockedOpen.complete(null);
      },
      destroy: () => destroyed++,
    );
    addTearDown(() {
      if (!blockedOpen.isCompleted) blockedOpen.complete(null);
      thumbnailer.dispose();
    });

    final running = thumbnailer.thumbnailAt(Duration.zero);
    thumbnailer.dispose();
    expect(cancelled, 1);
    expect(inFfi, isFalse);
    expect(destroyed, 0);

    enterFfi.complete();
    expect(await running.timeout(const Duration(seconds: 1)), isNull);
    expect(destroyed, 1);
    final callsAtDestroy = cancelled;
    thumbnailer.dispose();
    await Future<void>.delayed(const Duration(milliseconds: 75));
    expect(cancelled, callsAtDestroy);
    expect(destroyed, 1);
  });
}
