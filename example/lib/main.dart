import 'package:flutter/material.dart';
import 'package:audio_player/audio_player.dart';
import 'package:flutter/scheduler.dart';

String _playUrl = "http://music.163.com/song/media/outer/url?id=33894312.mp3";

void main() {
  runApp(MyApp());
}

class MyApp extends StatefulWidget {
  @override
  _MyAppState createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  AudioPlayer player;

  @override
  void initState() {
    super.initState();
    player = AudioPlayer.url(_playUrl);
    player.onStateChanged.addListener(() {
      debugPrint("state change: ${player.status} playing: ${player.isPlaying}");
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
        ),
        body: Center(
          child: Column(
            children: [
              _PlaybackStatefulButton(player: player),
              ProgressTrackingContainer(
                builder: (context) {
                  double progress;
                  if (player.duration > Duration.zero) {
                    progress = player.currentTime.inMilliseconds / player.duration.inMilliseconds;
                  }
                  return LinearProgressIndicator(
                    value: progress,
                  );
                },
                player: player,
              ),
              _ForwardRewindButton(player: player)
            ],
          ),
        ),
      ),
    );
  }
}

class _ForwardRewindButton extends StatelessWidget {
  final AudioPlayer player;
  const _ForwardRewindButton({Key key, this.player}) : super(key: key);
  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        IconButton(
            icon: Icon(Icons.replay_10),
            onPressed: () {
              final Duration to = player.currentTime - const Duration(seconds: 10);
              player.seek(to.atmost(player.duration));
            }),
        SizedBox(width: 20),
        IconButton(
            icon: Icon(Icons.forward_10),
            onPressed: () {
              final Duration to = player.currentTime + const Duration(seconds: 10);
              debugPrint("current = ${player.currentTime}");
              player.seek(to.atleast(Duration.zero));
            }),
      ],
    );
  }
}

extension _DurationClimp on Duration {
  Duration atmost(Duration duration) {
    return this <= duration ? this : duration;
  }

  Duration atleast(Duration duration) {
    return this >= duration ? this : duration;
  }
}

class _PlaybackStatefulButton extends StatelessWidget {
  final AudioPlayer player;
  const _PlaybackStatefulButton({Key key, this.player}) : super(key: key);
  @override
  Widget build(BuildContext context) {
    return IconButton(
      icon: AnimatedBuilder(
          animation: player.onStateChanged,
          builder: (context, child) {
            if (player.isPlaying) {
              return Icon(Icons.pause);
            } else if (player.status == PlayerStatus.Error) {
              return Icon(Icons.error);
            } else if (player.status == PlayerStatus.Buffering) {
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
        player.playWhenReady = !player.playWhenReady;
      },
    );
  }
}

class ProgressTrackingContainer extends StatefulWidget {
  final AudioPlayer player;
  final WidgetBuilder builder;

  const ProgressTrackingContainer({
    Key key,
    @required this.builder,
    @required this.player,
  })  : assert(builder != null),
        assert(player != null),
        super(key: key);

  @override
  _ProgressTrackingContainerState createState() => _ProgressTrackingContainerState();
}

class _ProgressTrackingContainerState extends State<ProgressTrackingContainer> with SingleTickerProviderStateMixin {
  AudioPlayer _player;

  Ticker _ticker;

  @override
  void initState() {
    super.initState();
    _player = widget.player..onStateChanged.addListener(_onStateChanged);
    _ticker = createTicker((elapsed) {
      setState(() {});
    });
    _onStateChanged();
  }

  void _onStateChanged() {
    final needTrack = widget.player.isPlaying;
    if (_ticker.isActive == needTrack) return;
    if (_ticker.isActive) {
      _ticker.stop();
    } else {
      _ticker.start();
    }
  }

  @override
  void dispose() {
    _player.onStateChanged.removeListener(_onStateChanged);
    _ticker.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
        animation: widget.player.onStateChanged,
        builder: (context, child) {
          return widget.builder(context);
        });
  }
}
