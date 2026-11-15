import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'player.dart';

/// How [LycheeVideo] draws text subtitles. Sizes are fractions of the
/// video's height, so the text scales with the picture.
@immutable
class LycheeSubtitleStyle {
  const LycheeSubtitleStyle({
    this.textStyle = const TextStyle(color: Color(0xFFFFFFFF)),
    this.fontSize = 0.05,
    this.outlineColor = const Color(0xFF000000),
    this.outlineWidth = 0.08,
    this.backgroundColor,
    this.bottomMargin = 0.05,
    this.textAlign = TextAlign.center,
  });

  /// Font, color, weight, shadows. Its font size is ignored: see
  /// [fontSize].
  final TextStyle textStyle;

  /// Of the video's height.
  final double fontSize;

  /// Drawn around each glyph, [outlineWidth] times the font size wide;
  /// null for none.
  final Color? outlineColor;
  final double outlineWidth;

  /// Behind the text of each line; null for none.
  final Color? backgroundColor;

  /// Between the last line and the bottom of the video, of its height.
  final double bottomMargin;
  final TextAlign textAlign;
}

/// Shows the video of [player], keeping its aspect ratio and rotation, with
/// its subtitles over it: the pictures of bitmap subtitles
/// ([LycheePlayerValue.subtitlePictures]), and text subtitles
/// ([LycheePlayerValue.subtitle]) in [subtitleStyle].
class LycheeVideo extends StatelessWidget {
  const LycheeVideo(
    this.player, {
    super.key,
    this.fit = BoxFit.contain,
    this.subtitleStyle = const LycheeSubtitleStyle(),
    this.surfaceView = false,
    this.onSurfaceError,
  });

  final LycheePlayer player;
  final BoxFit fit;

  /// Null draws no text subtitles, for drawing them yourself.
  final LycheeSubtitleStyle? subtitleStyle;

  /// Android: show the video in a SurfaceView below the app's Flutter
  /// view instead of a texture, for 4K and HDR on TVs. The compositor then
  /// shows the decoder's frames itself: at the panel's resolution where
  /// Flutter's UI is drawn smaller (4K TVs whose UI runs at 1080p), with
  /// HDR passed on to the display, and without the copy into Flutter's
  /// texture. The widget clears its video rectangle to transparent, so
  /// widgets painted over it show and those under it do not.
  ///
  /// It needs Flutter to draw through a transparent TextureView: override
  /// `getRenderMode()` in the app's `FlutterActivity` to return
  /// `RenderMode.texture` and set `view.isOpaque = false` in
  /// `onFlutterTextureViewCreated`. Without it, without bounded
  /// constraints, or for non-quarter-turn rotation, the video falls back to
  /// the texture unless [onSurfaceError] is set. Only one [LycheeVideo] with
  /// it should show a player at a time; the SurfaceView follows the widget a
  /// frame late when it moves. Entering this mode can briefly rebuffer while
  /// hardware decoding is recreated.
  final bool surfaceView;

  /// Android: when [surfaceView] is true, reports a native presentation
  /// failure (invalid layout, unsupported rotation, missing host setup,
  /// timeout, or bind/post error) instead of falling back to a Flutter
  /// texture. The widget also shows an actionable message; hosts can pause
  /// playback and expose their normal retry UI.
  final ValueChanged<String>? onSurfaceError;

  @override
  Widget build(BuildContext context) {
    return _Drawn(
      player,
      (context, drawn) {
        final size = drawn.size;
        if (size == null || size.isEmpty) {
          return const SizedBox.shrink();
        }
        final turns = (drawn.rotation % 360) ~/ 90;
        final rotated = turns.isOdd ? size.flipped : size;
        final pictures = drawn.pictures;
        final text = pictures == null ? drawn.subtitle : null;
        final style = subtitleStyle;
        // Video (or nothing) and subtitles, laid out in pixels of the
        // video and scaled by |fit|.
        Widget fitted(Widget? video) => FittedBox(
              fit: fit,
              child: SizedBox.fromSize(
                size: rotated,
                child: Stack(
                  children: [
                    if (video != null) video,
                    for (final picture in pictures ?? const [])
                      Positioned.fromRect(
                        rect: picture.rect,
                        child: RawImage(image: picture.image, fit: BoxFit.fill),
                      ),
                    if (text != null && style != null)
                      _SubtitleText(text, style, rotated),
                  ],
                ),
              ),
            );
        Widget textureView() =>
            ClipRect(child: fitted(_textureOf(size, rotated, turns)));
        if (surfaceView &&
            !kIsWeb &&
            defaultTargetPlatform == TargetPlatform.android) {
          return LayoutBuilder(builder: (context, constraints) {
            if (!constraints.hasBoundedWidth || !constraints.hasBoundedHeight) {
              if (onSurfaceError != null) {
                return _SurfaceFailure(
                  'SurfaceView requires bounded width and height. Retry after restoring the video layout.',
                  onSurfaceError,
                );
              }
              return textureView();
            }
            if (drawn.rotation % 90 != 0 ||
                !size.width.isFinite ||
                !size.height.isFinite) {
              if (onSurfaceError != null) {
                return _SurfaceFailure(
                  'SurfaceView requires a finite video size and quarter-turn rotation. Try another source.',
                  onSurfaceError,
                );
              }
              return textureView();
            }
            if (constraints.biggest.isEmpty) return const SizedBox.shrink();
            // The SurfaceView gets the video's rectangle untransformed: it
            // does not follow a scale.
            final box = Offset.zero & constraints.biggest;
            // cover crops the source; native placement still needs the full,
            // proportionally scaled video, clipped to the widget's viewport.
            final sizes = applyBoxFit(fit, rotated, box.size);
            if (sizes.source.isEmpty) return const SizedBox.shrink();
            final destination = sizes.destination;
            final fullSize = Size(
                rotated.width * destination.width / sizes.source.width,
                rotated.height * destination.height / sizes.source.height);
            final rect = Alignment.center.inscribe(fullSize, box);
            return _AndroidSurfaceView(
              key: ValueKey(player.textureId),
              textureId: player.textureId,
              boxSize: box.size,
              rect: rect,
              clipRect: box.shift(-rect.topLeft),
              overlay: fitted(null),
              fallback: onSurfaceError == null ? textureView() : null,
              onError: onSurfaceError,
            );
          });
        }
        return textureView();
      },
    );
  }

  Widget _textureOf(Size size, Size rotated, int turns) {
    if (!kIsWeb && defaultTargetPlatform == TargetPlatform.android) {
      // Android players enable rotate_surface_frames and an upright driver,
      // so SurfaceTexture already delivers frames rotated upright.
      return SizedBox.fromSize(
        size: rotated,
        child: Texture(textureId: player.textureId),
      );
    }
    return RotatedBox(
      quarterTurns: turns,
      child: SizedBox.fromSize(
        size: size,
        child: Texture(textureId: player.textureId),
      ),
    );
  }
}

// The player's SurfaceView (LycheeSurfaceView.kt), below Flutter's view:
// this clears its rectangle to transparent and tells the plugin where it
// is. Not a platform view: in Hybrid Composition, Android's Choreographer
// missed a vsync in eight, and the video lost a third of its frames on a
// TV. The texture instead where the app's Activity cannot show one.
class _AndroidSurfaceView extends StatefulWidget {
  const _AndroidSurfaceView({
    super.key,
    required this.textureId,
    required this.boxSize,
    required this.rect,
    required this.clipRect,
    required this.overlay,
    this.fallback,
    this.onError,
  });

  final int textureId;
  final Size boxSize;
  final Rect rect;
  final Rect clipRect;
  final Widget overlay;
  final Widget? fallback;
  final ValueChanged<String>? onError;

  @override
  State<_AndroidSurfaceView> createState() => _AndroidSurfaceViewState();
}

class _AndroidSurfaceViewState extends State<_AndroidSurfaceView> {
  static const _channel = MethodChannel('lychee_player');

  // Where the plugin was last told the view is, in pixels of Flutter's view.
  List<int>? _rect;
  List<int>? _clipRect;
  String? _error;
  bool _fallenBack = false;
  StreamSubscription<dynamic>? _events;
  int _placement = 0;

  @override
  void initState() {
    super.initState();
    _events = EventChannel('lychee_player/surface/${widget.textureId}')
        .receiveBroadcastStream()
        .listen((event) {
      if (event is Map && event['error'] is String) {
        _fail(event['error'] as String);
      }
    }, onError: (Object error) {
      if (widget.fallback != null && error is MissingPluginException) return;
      _fail('SurfaceView status unavailable. Restart the app and retry.');
    });
  }

  void _fail(String message) {
    if (!mounted) return;
    if (widget.fallback != null) {
      if (!_fallenBack) {
        _detach();
        setState(() => _fallenBack = true);
      }
      return;
    }
    if (_error != null) return;
    setState(() => _error = message);
    widget.onError?.call(message);
  }

  void _detach() {
    _placement++;
    _events?.cancel();
    _events = null;
    if (_rect != null) {
      _rect = null;
      _clipRect = null;
      _channel.invokeMethod<bool>('setSurfaceView', {
        'textureId': widget.textureId,
        'rect': null,
      }).catchError((Object _) => false);
    }
  }

  void _place(Rect rect, Rect clipRect) {
    final values = [
      rect.left,
      rect.top,
      rect.width,
      rect.height,
      clipRect.left,
      clipRect.top,
      clipRect.width,
      clipRect.height
    ];
    if (values.any((v) => !v.isFinite || v.abs() > 0x7fffffff) ||
        rect.isEmpty ||
        clipRect.isEmpty) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _fail(
          'SurfaceView has invalid physical bounds. Restore the video layout and retry.'));
      return;
    }
    final pixels = [
      rect.left.round(),
      rect.top.round(),
      rect.width.round(),
      rect.height.round(),
    ];
    final clipPixels = [
      clipRect.left.round(),
      clipRect.top.round(),
      clipRect.width.round(),
      clipRect.height.round()
    ];
    if (listEquals(pixels, _rect) && listEquals(clipPixels, _clipRect)) {
      return;
    }
    _rect = pixels;
    _clipRect = clipPixels;
    final placement = ++_placement;
    _channel.invokeMethod<bool>('setSurfaceView', {
      'textureId': widget.textureId,
      'rect': pixels,
      'clipRect': clipPixels,
    }).then((shown) {
      if (mounted && placement == _placement && shown != true) {
        _fail(
            'SurfaceView could not attach. Restart the app and retry; the host must use a transparent Flutter TextureView.');
      }
    }).catchError((Object error) {
      if (mounted && placement == _placement) {
        _fail('SurfaceView binding failed. Restart the app and retry.');
      }
    });
  }

  @override
  void dispose() {
    _detach();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_fallenBack && widget.fallback != null) return widget.fallback!;
    if (_error != null) return _SurfaceFailure(_error!, null);
    return ClipRect(
      child: SizedBox.fromSize(
        size: widget.boxSize,
        child: Stack(
          children: [
            Positioned.fromRect(
              rect: widget.rect,
              child: _SurfaceViewHole(_place, widget.clipRect),
            ),
            Positioned.fill(child: widget.overlay),
          ],
        ),
      ),
    );
  }
}

class _SurfaceFailure extends StatefulWidget {
  const _SurfaceFailure(this.message, this.onError);
  final String message;
  final ValueChanged<String>? onError;

  @override
  State<_SurfaceFailure> createState() => _SurfaceFailureState();
}

class _SurfaceFailureState extends State<_SurfaceFailure> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) widget.onError?.call(widget.message);
    });
  }

  @override
  Widget build(BuildContext context) => Text(widget.message,
      textDirection: TextDirection.ltr,
      style: const TextStyle(color: Color(0xFFFFFFFF), fontSize: 16));
}

class _SurfaceViewHole extends LeafRenderObjectWidget {
  const _SurfaceViewHole(this.onPaint, this.clipRect);

  final void Function(Rect, Rect) onPaint;
  final Rect clipRect;

  @override
  RenderObject createRenderObject(BuildContext context) =>
      _RenderSurfaceViewHole(
          onPaint, clipRect, View.of(context).devicePixelRatio);

  @override
  void updateRenderObject(
          BuildContext context, _RenderSurfaceViewHole renderObject) =>
      renderObject
        ..onPaint = onPaint
        ..clipRect = clipRect
        ..markNeedsPaint()
        ..pixelRatio = View.of(context).devicePixelRatio;
}

class _RenderSurfaceViewHole extends RenderBox {
  _RenderSurfaceViewHole(this.onPaint, this.clipRect, this._pixelRatio);

  void Function(Rect, Rect) onPaint;
  Rect clipRect;
  double _pixelRatio;
  set pixelRatio(double value) {
    if (value != _pixelRatio) {
      _pixelRatio = value;
      markNeedsPaint();
    }
  }

  static final _clear = Paint()..blendMode = BlendMode.clear;

  @override
  bool get sizedByParent => true;

  @override
  Size computeDryLayout(BoxConstraints constraints) => constraints.biggest;

  @override
  void paint(PaintingContext context, Offset offset) {
    context.canvas.drawRect(offset & size, _clear);
    // The root's coordinates are logical pixels.
    final rect =
        MatrixUtils.transformRect(getTransformTo(null), Offset.zero & size);
    final clip = MatrixUtils.transformRect(getTransformTo(null), clipRect);
    Rect physical(Rect r) => Rect.fromLTRB(r.left * _pixelRatio,
        r.top * _pixelRatio, r.right * _pixelRatio, r.bottom * _pixelRatio);
    onPaint(physical(rect), physical(clip));
  }
}

// What LycheeVideo draws of the player's value.
typedef _DrawnValue = ({
  Size? size,
  int rotation,
  String? subtitle,
  List<LycheeSubtitlePicture>? pictures,
});

_DrawnValue _drawnOf(LycheePlayerValue value) => (
      size: value.videoSize,
      rotation: value.rotation,
      subtitle: value.subtitle,
      pictures: value.subtitlePictures,
    );

// Builds when what LycheeVideo draws changes, not on every change of the
// player's value (its position moves several times a second).
class _Drawn extends StatefulWidget {
  const _Drawn(this.player, this.builder);

  final LycheePlayer player;
  final Widget Function(BuildContext context, _DrawnValue drawn) builder;

  @override
  State<_Drawn> createState() => _DrawnState();
}

class _DrawnState extends State<_Drawn> {
  late _DrawnValue _drawn = _drawnOf(widget.player.value);

  @override
  void initState() {
    super.initState();
    widget.player.addListener(_onChanged);
  }

  @override
  void didUpdateWidget(_Drawn oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.player != widget.player) {
      oldWidget.player.removeListener(_onChanged);
      widget.player.addListener(_onChanged);
      _drawn = _drawnOf(widget.player.value);
    }
  }

  @override
  void dispose() {
    widget.player.removeListener(_onChanged);
    super.dispose();
  }

  void _onChanged() {
    final drawn = _drawnOf(widget.player.value);
    // Records compare field by field; the pictures list by identity.
    if (drawn != _drawn) {
      setState(() => _drawn = drawn);
    }
  }

  @override
  Widget build(BuildContext context) => widget.builder(context, _drawn);
}

// Laid out in pixels of the video, which the FittedBox above scales.
class _SubtitleText extends StatefulWidget {
  const _SubtitleText(this.text, this.style, this.video);

  final String text;
  final LycheeSubtitleStyle style;
  final Size video;

  @override
  State<_SubtitleText> createState() => _SubtitleTextState();
}

class _SubtitleTextState extends State<_SubtitleText> {
  // Kept while the style and the video's height stay the same: a Paint has
  // no ==, so a new one each build would lay the text out and paint it
  // again even when it did not change.
  late TextStyle _fill;
  TextStyle? _under;

  @override
  void initState() {
    super.initState();
    _makeStyles();
  }

  @override
  void didUpdateWidget(_SubtitleText oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.style != widget.style ||
        oldWidget.video.height != widget.video.height) {
      _makeStyles();
    }
  }

  void _makeStyles() {
    final style = widget.style;
    final fontSize = style.fontSize * widget.video.height;
    _fill = style.textStyle.copyWith(
      fontSize: fontSize,
      decoration: TextDecoration.none,
    );
    final outline = style.outlineColor;
    final background = style.backgroundColor;
    if (outline != null) {
      _under = _fill.copyWith(
        foreground: Paint()
          ..style = PaintingStyle.stroke
          ..strokeJoin = StrokeJoin.round
          ..strokeWidth = style.outlineWidth * fontSize
          ..color = outline,
        backgroundColor: background,
      );
    } else if (background != null) {
      _under = _fill.copyWith(backgroundColor: background);
    } else {
      _under = null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final style = widget.style;
    final video = widget.video;
    final under = _under;
    Widget line(TextStyle textStyle) =>
        Text(widget.text, textAlign: style.textAlign, style: textStyle);
    return Positioned(
      left: video.width * 0.05,
      right: video.width * 0.05,
      bottom: style.bottomMargin * video.height,
      // The app's text style (an underline outside a Material) stays out.
      child: DefaultTextStyle(
        style: const TextStyle(),
        child: Stack(
          alignment: Alignment.bottomCenter,
          children: [
            if (under != null) line(under),
            line(_fill),
          ],
        ),
      ),
    );
  }
}
