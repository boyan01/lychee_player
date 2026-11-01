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
  });

  final LycheePlayer player;
  final BoxFit fit;

  /// Null draws no text subtitles, for drawing them yourself.
  final LycheeSubtitleStyle? subtitleStyle;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<LycheePlayerValue>(
      valueListenable: player,
      builder: (context, value, _) {
        final size = value.videoSize;
        if (size == null || size.isEmpty) {
          return const SizedBox.shrink();
        }
        final turns = value.rotation ~/ 90;
        final rotated = turns.isOdd ? size.flipped : size;
        final pictures = value.subtitlePictures;
        final text = pictures == null ? value.subtitle : null;
        final style = subtitleStyle;
        return ClipRect(
          child: FittedBox(
            fit: fit,
            child: SizedBox.fromSize(
              size: rotated,
              child: Stack(
                children: [
                  RotatedBox(
                    quarterTurns: turns,
                    child: SizedBox.fromSize(
                      size: size,
                      child: Texture(textureId: player.textureId),
                    ),
                  ),
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
          ),
        );
      },
    );
  }
}

// Laid out in pixels of the video, which the FittedBox above scales.
class _SubtitleText extends StatelessWidget {
  const _SubtitleText(this.text, this.style, this.video);

  final String text;
  final LycheeSubtitleStyle style;
  final Size video;

  @override
  Widget build(BuildContext context) {
    final fontSize = style.fontSize * video.height;
    final fill = style.textStyle.copyWith(
      fontSize: fontSize,
      decoration: TextDecoration.none,
    );
    final outline = style.outlineColor;
    final background = style.backgroundColor;
    Widget line(TextStyle textStyle) =>
        Text(text, textAlign: style.textAlign, style: textStyle);
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
            if (outline != null)
              line(fill.copyWith(
                foreground: Paint()
                  ..style = PaintingStyle.stroke
                  ..strokeJoin = StrokeJoin.round
                  ..strokeWidth = style.outlineWidth * fontSize
                  ..color = outline,
                backgroundColor: background,
              ))
            else if (background != null)
              line(fill.copyWith(backgroundColor: background)),
            line(fill),
          ],
        ),
      ),
    );
  }
}
