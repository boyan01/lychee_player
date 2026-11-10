import 'dart:convert';
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'bindings.dart' as native;

/// How much the library logs; each level includes the ones before it.
enum LycheeLogLevel { error, warning, info, debug, verbose }

/// The library's own log: decoders opened and rejected, network retries,
/// stalls. By default it goes to the platform's log (logcat on Android,
/// stderr elsewhere).
abstract final class LycheeLog {
  static void Function(LycheeLogLevel level, String message)? _handler;
  static NativeCallable<native.LogListenerNative>? _listener;

  /// Logs [level] and the levels before it. The default is
  /// [LycheeLogLevel.info].
  static set level(LycheeLogLevel level) => native.setLogLevel(level.index);

  /// Sends the library's log lines to [handler] on this isolate, instead of
  /// the platform's log; null sends them back there. Each line starts with
  /// its level and the part of the library it comes from, e.g.
  /// `[W/hls] ...`. Lines arrive a little after they were logged, in order
  /// per thread. Returns false, and changes nothing, with a core that
  /// cannot (one older than this package).
  static bool setHandler(
    void Function(LycheeLogLevel level, String message)? handler,
  ) {
    final setListener = native.setLogListener;
    final free = native.freeLogMessage;
    if (setListener == null || free == null) {
      return false;
    }
    _handler = handler;
    if (handler == null) {
      setListener(nullptr, nullptr);
      return true;
    }
    // Kept open for the life of the isolate: lines already on their way
    // still arrive (and are freed) after the handler changes.
    final listener =
        _listener ??= NativeCallable<native.LogListenerNative>.listener((
      Pointer<Void> _,
      int level,
      int message,
    ) {
      final pointer = Pointer<Utf8>.fromAddress(message);
      final String text;
      try {
        // A line cut at the library's line length may end mid-character.
        text = utf8.decode(
          pointer.cast<Uint8>().asTypedList(pointer.length),
          allowMalformed: true,
        );
      } finally {
        free(message);
      }
      _handler?.call(
        LycheeLogLevel.values[level.clamp(
          0,
          LycheeLogLevel.values.length - 1,
        )],
        text,
      );
    });
    setListener(listener.nativeFunction, nullptr);
    return true;
  }
}
