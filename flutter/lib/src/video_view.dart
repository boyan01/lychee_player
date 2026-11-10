import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
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
  });

  final LycheePlayer player;
  final BoxFit fit;

  /// Null draws no text subtitles, for drawing them yourself.
  final LycheeSubtitleStyle? subtitleStyle;

  /// Android: show the video in a SurfaceView (a platform view, in Hybrid
  /// Composition) instead of a texture, for 4K and HDR on TVs. The
  /// compositor then shows the decoder's frames itself: at the panel's
  /// resolution where Flutter's UI is drawn smaller (4K TVs whose UI runs
  /// at 1080p), with HDR passed on to the display, and without the copy
  /// into Flutter's texture. Widgets over the video cost more to draw
  /// (Flutter composites them in layers of their own). Only one
  /// [LycheeVideo] with it should show a player at a time. Ignored on other
  /// platforms, for rotated video, and without bounded constraints.
  final bool surfaceView;

  @override
  Widget build(BuildContext context) {
    return _Drawn(
      player,
      (context, drawn) {
        final size = drawn.size;
        if (size == null || size.isEmpty) {
          return const SizedBox.shrink();
        }
        final turns = drawn.rotation ~/ 90;
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
        if (surfaceView &&
            turns == 0 &&
            !kIsWeb &&
            defaultTargetPlatform == TargetPlatform.android) {
          return LayoutBuilder(builder: (context, constraints) {
            if (!constraints.hasBoundedWidth || !constraints.hasBoundedHeight) {
              return ClipRect(child: fitted(_textureOf(size, turns)));
            }
            // The platform view gets the video's rectangle untransformed:
            // a SurfaceView does not follow a scale.
            final box = Offset.zero & constraints.biggest;
            final destination = applyBoxFit(fit, size, box.size).destination;
            final rect = Alignment.center.inscribe(destination, box);
            return ClipRect(
              child: SizedBox.fromSize(
                size: box.size,
                child: Stack(
                  children: [
                    Positioned.fromRect(
                      rect: rect,
                      child: _AndroidSurfaceView(
                        key: ValueKey(player.textureId),
                        textureId: player.textureId,
                      ),
                    ),
                    Positioned.fill(child: fitted(null)),
                  ],
                ),
              ),
            );
          });
        }
        return ClipRect(child: fitted(_textureOf(size, turns)));
      },
    );
  }

  Widget _textureOf(Size size, int turns) => RotatedBox(
        quarterTurns: turns,
        child: SizedBox.fromSize(
          size: size,
          child: Texture(textureId: player.textureId),
        ),
      );
}

// The player's SurfaceView (LycheeSurfaceView.kt). Hybrid Composition: the
// SurfaceView sits in Android's view hierarchy; texture layer composition
// would draw it into a texture again.
class _AndroidSurfaceView extends StatelessWidget {
  const _AndroidSurfaceView({super.key, required this.textureId});

  final int textureId;

  static const _viewType = 'lychee_player/surface_view';

  @override
  Widget build(BuildContext context) {
    return PlatformViewLink(
      viewType: _viewType,
      surfaceFactory: (context, controller) => AndroidViewSurface(
        controller: controller as AndroidViewController,
        gestureRecognizers: const <Factory<OneSequenceGestureRecognizer>>{},
        hitTestBehavior: PlatformViewHitTestBehavior.transparent,
      ),
      onCreatePlatformView: (params) =>
          PlatformViewsService.initExpensiveAndroidView(
        id: params.id,
        viewType: _viewType,
        layoutDirection: TextDirection.ltr,
        creationParams: {'textureId': textureId},
        creationParamsCodec: const StandardMessageCodec(),
      )
            ..addOnPlatformViewCreatedListener(params.onPlatformViewCreated)
            ..create(),
    );
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
