import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:ffi/ffi.dart';

import 'bindings.dart' as native;

/// Pictures of a file or URL at given positions, for seek bar previews
/// where the media has no sprite sheet: decoded apart from any player,
/// with a demuxer and a decoder of its own. The input opens with the first
/// [thumbnailAt] and stays open for the next.
///
/// Decoding runs in a background isolate, off the UI isolate's event loop,
/// one request at a time: a request still waiting when a newer one comes
/// completes with null, so a seek bar dragged along gets the newest
/// position soon.
class LycheeThumbnailer {
  LycheeThumbnailer._(this._handle);

  /// Thumbnails of [url], whose http(s) requests carry [headers] as for
  /// `LycheePlayer.open`, fitted to [maxSize] in pixels (null: the video's
  /// own size) keeping their aspect ratio, never enlarged. Opens nothing
  /// yet.
  ///
  /// Throws an [UnsupportedError] with a core older than API 30, and an
  /// [ArgumentError] for a malformed header.
  factory LycheeThumbnailer.create(String url,
      {Map<String, String>? headers, ui.Size? maxSize}) {
    final create = native.thumbnailerCreate;
    if (create == null) {
      throw UnsupportedError('thumbnails need a lychee core of API 30');
    }
    final lines = native.headerLines(headers);
    final nativeUrl = url.toNativeUtf8();
    final handle = native.withOpenOptions(
        lines,
        null,
        (options) => create(nativeUrl.cast(), options,
            maxSize?.width.round() ?? 0, maxSize?.height.round() ?? 0));
    malloc.free(nativeUrl);
    if (handle == nullptr) {
      throw ArgumentError.value(url, 'url', 'rejected');
    }
    return LycheeThumbnailer._(handle);
  }

  final Pointer<Void> _handle;
  _Request? _waiting;
  bool _running = false;
  bool _disposed = false;

  /// The picture at [position], upright. Without [exact], the keyframe at
  /// or before it, which is quick; with it, the frame showing there. Null
  /// if the input cannot be opened, has no video, cannot seek there or be
  /// decoded (the log says why), after [dispose], or when a newer request
  /// replaced this one before it started.
  Future<ui.Image?> thumbnailAt(Duration position, {bool exact = false}) {
    if (_disposed) {
      return Future.value(null);
    }
    _waiting?.completer.complete(null);
    final request = _Request(position, exact);
    _waiting = request;
    _run();
    return request.completer.future;
  }

  Future<void> _run() async {
    if (_running) {
      return;
    }
    _running = true;
    while (!_disposed && _waiting != null) {
      final request = _waiting!;
      _waiting = null;
      // The pixels are copied there: nothing native comes back.
      final picture = await Isolate.run(_task(
          _handle.address, request.position.inMicroseconds, request.exact));
      request.completer.complete(
          picture == null || _disposed ? null : await _image(picture));
    }
    _running = false;
    if (_disposed) {
      native.thumbnailerDestroy(_handle);
    }
  }

  /// Cancels the request in progress (its future completes with null) and
  /// releases the thumbnailer once it has returned.
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _waiting?.completer.complete(null);
    _waiting = null;
    if (_running) {
      native.thumbnailerCancel(_handle);
    } else {
      native.thumbnailerDestroy(_handle);
    }
  }
}

class _Request {
  _Request(this.position, this.exact);

  final Duration position;
  final bool exact;
  final completer = Completer<ui.Image?>();
}

typedef _Picture = ({Uint8List pixels, int width, int height, int stride});

// A closure that captures only what it is given, for Isolate.run().
_Picture? Function() _task(int handle, int micros, bool exact) =>
    () => _get(handle, micros, exact);

// In a background isolate.
_Picture? _get(int handle, int micros, bool exact) {
  final frame = native.thumbnailerGet(
      Pointer<Void>.fromAddress(handle), micros, exact ? 1 : 0);
  if (frame == nullptr) {
    return null;
  }
  final stride = calloc<Int32>();
  try {
    final data = native.frameGetData(frame, stride);
    if (native.frameGetType(frame) != native.kFrameCpuRgba || data == nullptr) {
      return null;
    }
    final height = native.frameGetHeight(frame);
    return (
      pixels: Uint8List.fromList(data.asTypedList(stride.value * height)),
      width: native.frameGetWidth(frame),
      height: height,
      stride: stride.value,
    );
  } finally {
    calloc.free(stride);
    native.frameRelease(frame);
  }
}

Future<ui.Image> _image(_Picture picture) async {
  final buffer = await ui.ImmutableBuffer.fromUint8List(picture.pixels);
  final descriptor = ui.ImageDescriptor.raw(buffer,
      width: picture.width,
      height: picture.height,
      rowBytes: picture.stride,
      pixelFormat: ui.PixelFormat.rgba8888);
  final codec = await descriptor.instantiateCodec();
  try {
    return (await codec.getNextFrame()).image;
  } finally {
    codec.dispose();
    descriptor.dispose();
    buffer.dispose();
  }
}
