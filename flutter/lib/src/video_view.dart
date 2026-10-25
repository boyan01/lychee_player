import 'package:flutter/widgets.dart';

import 'player.dart';

/// Shows the video of [player], keeping its aspect ratio and rotation, with
/// its subtitle pictures ([LycheePlayerValue.subtitlePictures]) over it.
class LycheeVideo extends StatelessWidget {
  const LycheeVideo(this.player, {super.key, this.fit = BoxFit.contain});

  final LycheePlayer player;
  final BoxFit fit;

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
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}
