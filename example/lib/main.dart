import 'package:audio_player/audio_player.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

void main() {
  runApp(MyApp());
}

enum _Type { file, url, asset }

const Map<String, _Type> urls = {
  "http://music.163.com/song/media/outer/url?id=33894312.mp3": _Type.url,
  "tracks/rise.mp3": _Type.asset,
  "https://storage.googleapis.com/exoplayer-test-media-0/play.mp3": _Type.url,
};

class MyApp extends StatefulWidget {
  @override
  _MyAppState createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  AudioPlayer player;
  String url;

  @override
  void initState() {
    super.initState();
    _newPlayer(urls.keys.first);
  }

  void _newPlayer(String uri) {
    if (this.url == uri) {
      return;
    }
    if (player != null) {
      player.dispose();
    }
    final type = urls[uri];
    assert(type != null);
    switch (type) {
      case _Type.file:
        player = AudioPlayer.file(uri);
        break;
      case _Type.url:
        player = AudioPlayer.url(uri);
        break;
      case _Type.asset:
        player = AudioPlayer.asset(uri);
        break;
    }
    this.url = uri;
    player.onStateChanged.addListener(() {
      debugPrint("state change: ${player.status} playing: ${player.isPlaying}");
    });
    player.playWhenReady = true;
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
        ),
        body: Column(
          children: [
            Spacer(),
            _PlayerUi(player: player, url: this.url),
            Spacer(),
            for (var item in urls.keys)
              RadioListTile(
                value: item,
                groupValue: url,
                title: Text(item),
                onChanged: (newValue) {
                  setState(() {
                    _newPlayer(newValue);
                  });
                },
              ),
            Spacer(),
          ],
        ),
      ),
    );
  }
}

class _PlayerUi extends StatelessWidget {
  const _PlayerUi({
    Key key,
    @required this.player,
    @required this.url,
  }) : super(key: key);

  final AudioPlayer player;
  final String url;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            "playing: $url",
            softWrap: false,
            overflow: TextOverflow.fade,
          ),
          _PlaybackStatefulButton(player: player),
          ProgressTrackingContainer(
            builder: (context) {
              double progress;
              if (player.duration > Duration.zero) {
                progress = player.currentTime.inMilliseconds /
                    player.duration.inMilliseconds;
              }
              return LinearProgressIndicator(
                value: progress,
              );
            },
            player: player,
          ),
          SizedBox(height: 8),
          _PlayerBufferedRangeIndicator(player: player),
          _ForwardRewindButton(player: player)
        ],
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
              final Duration to =
                  player.currentTime - const Duration(seconds: 10);
              player.seek(to.atMost(player.duration));
            }),
        SizedBox(width: 20),
        IconButton(
            icon: Icon(Icons.forward_10),
            onPressed: () {
              final Duration to =
                  player.currentTime + const Duration(seconds: 10);
              debugPrint("current = ${player.currentTime}");
              player.seek(to.atLeast(Duration.zero));
            }),
      ],
    );
  }
}

extension _DurationClimp on Duration {
  Duration atMost(Duration duration) {
    return this <= duration ? this : duration;
  }

  Duration atLeast(Duration duration) {
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
            } else if (player.hasError) {
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
        if (player.status == PlayerStatus.End) {
          player.seek(Duration.zero);
        }
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
  _ProgressTrackingContainerState createState() =>
      _ProgressTrackingContainerState();
}

class _ProgressTrackingContainerState extends State<ProgressTrackingContainer>
    with SingleTickerProviderStateMixin {
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

  @override
  void didUpdateWidget(covariant ProgressTrackingContainer oldWidget) {
    super.didUpdateWidget(oldWidget);
    _player?.onStateChanged?.removeListener(_onStateChanged);
    _player = widget.player..onStateChanged.addListener(_onStateChanged);
    _onStateChanged();
  }

  void _onStateChanged() {
    final needTrack = true;
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

class _PlayerBufferedRangeIndicator extends StatefulWidget {
  final AudioPlayer player;

  const _PlayerBufferedRangeIndicator({
    Key key,
    @required this.player,
  }) : super(key: key);

  @override
  _PlayerBufferedRangeIndicatorState createState() =>
      _PlayerBufferedRangeIndicatorState();
}

class _PlayerBufferedRangeIndicatorState
    extends State<_PlayerBufferedRangeIndicator> {
  AudioPlayer _player;
  Duration _duration;

  AudioPlayerDisposable _disposable;

  @override
  void initState() {
    super.initState();
    _player = widget.player;
    _initPlayer();
  }

  @override
  void didUpdateWidget(covariant _PlayerBufferedRangeIndicator oldWidget) {
    super.didUpdateWidget(oldWidget);
    _player = widget.player;
    _initPlayer();
  }

  void _initPlayer() {
    _disposable?.dispose();
    _disposable = _player.onReady(() {
      setState(() {
        _duration = _player.duration;
      });
    });
  }

  @override
  void dispose() {
    _disposable?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    Widget indicator;
    if (_duration != null) {
      indicator = AnimatedBuilder(
          animation: _player.buffered,
          builder: (context, snapshot) {
            debugPrint("_player.buffered = ${_player.buffered.value}");
            return CustomPaint(
              painter: _PlayerBufferedRangeIndicatorPainter(
                  _player.buffered.value,
                  _duration,
                  Colors.transparent,
                  Theme.of(context).primaryColor),
            );
          });
    }
    return Container(
      constraints: BoxConstraints(
        minWidth: double.infinity,
        minHeight: 4.0,
      ),
      child: indicator,
    );
  }
}

class _PlayerBufferedRangeIndicatorPainter extends CustomPainter {
  final List<DurationRange> ranges;
  final Duration duration;

  final Color backgroundColor;
  final Color valueColor;

  _PlayerBufferedRangeIndicatorPainter(
      this.ranges, this.duration, this.backgroundColor, this.valueColor);

  @override
  void paint(Canvas canvas, Size size) {
    final Paint paint = Paint()
      ..color = backgroundColor
      ..style = PaintingStyle.fill;
    canvas.drawRect(Offset.zero & size, paint);

    paint.color = valueColor;

    void drawBar(double startFraction, double endFraction) {
      if (endFraction <= startFraction) {
        return;
      }
      canvas.drawRect(
          Offset(size.width * startFraction, 0) &
              Size(size.width * (endFraction - startFraction), size.height),
          paint);
    }

    ranges.forEach((e) {
      drawBar(e.startFraction(duration), e.endFraction(duration));
    });
  }

  @override
  bool shouldRepaint(
      covariant _PlayerBufferedRangeIndicatorPainter oldDelegate) {
    return duration != oldDelegate.duration ||
        ranges != oldDelegate.ranges ||
        backgroundColor != oldDelegate.backgroundColor ||
        valueColor != oldDelegate.valueColor;
  }
}
