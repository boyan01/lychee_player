import 'package:flutter/material.dart';
import 'package:lychee_player/lychee_player.dart';

extension DurationClimp on Duration {
  Duration atMost(Duration duration) {
    return this <= duration ? this : duration;
  }

  Duration atLeast(Duration duration) {
    return this >= duration ? this : duration;
  }
}

class ForwardRewindButton extends StatelessWidget {
  final AudioPlayer? player;

  const ForwardRewindButton({Key? key, this.player}) : super(key: key);

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        IconButton(
            icon: Icon(Icons.replay_10),
            onPressed: () {
              final Duration to =
                  player!.currentTime - const Duration(seconds: 10);
              player!.seek(to.atMost(player!.duration));
            }),
        SizedBox(width: 20),
        IconButton(
            icon: Icon(Icons.forward_10),
            onPressed: () {
              final Duration to =
                  player!.currentTime + const Duration(seconds: 10);
              debugPrint("current = ${player!.currentTime}");
              player!.seek(to.atLeast(Duration.zero));
            }),
      ],
    );
  }
}

class PlaybackStatefulButton extends StatelessWidget {
  final AudioPlayer? player;

  const PlaybackStatefulButton({Key? key, this.player}) : super(key: key);

  @override
  Widget build(BuildContext context) {
    return IconButton(
      icon: AnimatedBuilder(
          animation: player!.onStateChanged,
          builder: (context, child) {
            if (player!.isPlaying) {
              return Icon(Icons.pause);
            } else if (player!.hasError) {
              return Icon(Icons.error);
            } else if (player!.status == PlayerStatus.Buffering) {
              return Container(
                height: 24,
                width: 24,
                child: CircularProgressIndicator(),
              );
            } else {
              return Icon(Icons.play_arrow);
            }
          }),
      onPressed: () {
        if (player!.status == PlayerStatus.End) {
          player!.seek(Duration.zero);
        }
        player!.playWhenReady = !player!.playWhenReady;
      },
    );
  }
}
