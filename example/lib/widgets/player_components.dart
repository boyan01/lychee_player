import 'package:flutter/material.dart';
import 'package:lychee_player/lychee_player.dart';

extension DurationClimp on double {
  double atMost(double duration) {
    return this <= duration ? this : duration;
  }

  double atLeast(double duration) {
    return this >= duration ? this : duration;
  }
}

class PlayerControls extends StatefulWidget {
  const PlayerControls({
    Key? key,
    required this.player,
  }) : super(key: key);

  final LycheeAudioPlayer player;

  @override
  State<PlayerControls> createState() => _PlayerControlsState();
}

class _PlayerControlsState extends State<PlayerControls> {
  double _volume = 0;

  @override
  void initState() {
    super.initState();
    _volume = widget.player.volume;
  }

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Slider(
          value: _volume,
          onChanged: (value) {
            setState(() {
              _volume = value;
              widget.player.volume = value;
            });
          },
        ),
        SizedBox(width: 20),
        IconButton(
            icon: Icon(Icons.replay_10),
            onPressed: () {
              final to = widget.player.currentTime() - 10;
              widget.player.seek(to.atMost(widget.player.duration()));
            }),
        SizedBox(width: 20),
        IconButton(
            icon: Icon(Icons.forward_10),
            onPressed: () {
              final to = widget.player.currentTime() + 10;
              debugPrint("current = ${widget.player.currentTime()}");
              widget.player.seek(to.atLeast(0));
            }),
      ],
    );
  }
}

class PlaybackStatefulButton extends StatelessWidget {
  const PlaybackStatefulButton({Key? key, required this.player})
      : super(key: key);

  final LycheeAudioPlayer player;

  @override
  Widget build(BuildContext context) {
    return IconButton(
      icon: AnimatedBuilder(
          animation: Listenable.merge([
            player.state,
            player.onPlayWhenReadyChanged,
          ]),
          builder: (context, child) {
            if (player.state.value == PlayerState.ready) {
              if (player.playWhenReady) {
                return Icon(Icons.pause);
              } else {
                return Icon(Icons.play_arrow);
              }
            } else if (player.state.value == PlayerState.buffering) {
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
        if (player.state == PlayerState.end) {
          player.seek(0);
        }
        player.playWhenReady = !player.playWhenReady;
      },
    );
  }
}
