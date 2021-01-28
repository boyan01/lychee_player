import 'dart:math' as math;

import 'package:audio_player/audio_player.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'widgets/player_components.dart';

void main() {
  runApp(MyApp());
}

enum _Type { file, url, asset }

const Map<String, _Type> urls = {
  "http://music.163.com/song/media/outer/url?id=1451998397.mp3": _Type.url,
  "tracks/rise.mp3": _Type.asset,
  "https://storage.googleapis.com/exoplayer-test-media-0/play.mp3": _Type.url,
};

class MyApp extends StatefulWidget {
  @override
  _MyAppState createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  AudioPlayer? player;
  String? url;

  @override
  void initState() {
    super.initState();
    _newPlayer(urls.keys.first);
  }

  void _newPlayer(String? uri) {
    if (this.url == uri) {
      return;
    }
    if (player != null) {
      player!.dispose();
    }
    final type = urls[uri!]!;
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
    player!.onStateChanged.addListener(() {
      debugPrint(
          "state change: ${player!.status} playing: ${player!.isPlaying}");
    });
    player!.playWhenReady = true;
  }

  @override
  void dispose() {
    super.dispose();
    player?.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('Plugin example app')),
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
                onChanged: (dynamic newValue) {
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
    Key? key,
    required this.player,
    required this.url,
  }) : super(key: key);

  final AudioPlayer? player;
  final String? url;

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
          PlaybackStatefulButton(player: player),
          TickedPlayerState(player: player!),
          ForwardRewindButton(player: player)
        ],
      ),
    );
  }
}

class TickedPlayerState extends StatefulWidget {
  final AudioPlayer player;

  const TickedPlayerState({Key? key, required this.player}) : super(key: key);

  @override
  _TickedPlayerStateState createState() => _TickedPlayerStateState();
}

class _TickedPlayerStateState extends State<TickedPlayerState>
    with SingleTickerProviderStateMixin {
  late Ticker _ticker;

  @override
  void initState() {
    _ticker = createTicker((elapsed) {
      setState(() {});
    });
    _ticker.start();
    super.initState();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  Widget _buildProgress() {
    double? progress;
    if (widget.player.duration > Duration.zero) {
      progress = widget.player.currentTime.inMilliseconds /
          widget.player.duration.inMilliseconds;
    }
    return LinearProgressIndicator(
      value: progress,
    );
  }

  Widget _buildBufferedProgress() {
    double? progress;
    if (widget.player.duration > Duration.zero) {
      Duration bufferPosition = widget.player.buffered.value.max;
      if (bufferPosition <= Duration.zero) {
        progress = null;
      } else {
        progress = bufferPosition.inMilliseconds /
            widget.player.duration.inMilliseconds;
      }
    }
    return LinearProgressIndicator(
      value: progress,
    );
  }

  Widget _buildProgressText() {
    return Text(
        " ${(widget.player.currentTime.inMilliseconds / 1000.0).toStringAsFixed(2)}"
        "/${(widget.player.duration.inMilliseconds / 1000.0).toStringAsFixed(2)}"
        " - ${(widget.player.buffered.value.max.inMilliseconds / 1000).toStringAsFixed(2)}");
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        _buildProgress(),
        SizedBox(height: 16),
        _buildBufferedProgress(),
        SizedBox(height: 16),
        Align(
            alignment: AlignmentDirectional.centerEnd,
            child: _buildProgressText()),
      ],
    );
  }
}
