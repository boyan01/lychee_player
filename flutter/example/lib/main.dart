import 'dart:async';

import 'package:flutter/material.dart';
import 'package:lychee_player/lychee_player.dart';

void main() => runApp(const ExampleApp());

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'lychee_player',
      theme: ThemeData(colorSchemeSeed: Colors.deepOrange),
      home: const PlayerPage(),
    );
  }
}

class PlayerPage extends StatefulWidget {
  const PlayerPage({super.key});

  @override
  State<PlayerPage> createState() => _PlayerPageState();
}

class _PlayerPageState extends State<PlayerPage> {
  final _url = TextEditingController(
    text: const String.fromEnvironment('LYCHEE_URL'),
  );
  LycheePlayer? _player;

  @override
  void initState() {
    super.initState();
    if (_url.text.isNotEmpty) {
      _open();
    }
  }

  Future<void> _open() async {
    final player = await LycheePlayer.create();
    if (!mounted) {
      player.dispose();
      return;
    }
    player.open(_url.text.trim());
    player.play();
    // Dispose whatever is current now, not before the await: two opens in
    // flight would otherwise both replace the same player and leak one.
    final previous = _player;
    setState(() => _player = player);
    previous?.dispose();
  }

  @override
  void dispose() {
    _player?.dispose();
    _url.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final player = _player;
    return Scaffold(
      body: SafeArea(
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.all(8),
              child: Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: _url,
                      decoration: const InputDecoration(
                        hintText: 'File path or URL',
                        isDense: true,
                      ),
                      onSubmitted: (_) => _open(),
                    ),
                  ),
                  const SizedBox(width: 8),
                  FilledButton(onPressed: _open, child: const Text('Open')),
                ],
              ),
            ),
            Expanded(
              child: ColoredBox(
                color: Colors.black,
                child: player == null
                    ? const SizedBox.expand()
                    : Stack(
                        children: [
                          Center(child: LycheeVideo(player)),
                          _Subtitle(player),
                        ],
                      ),
              ),
            ),
            if (player != null) _Controls(player),
          ],
        ),
      ),
    );
  }
}

class _Controls extends StatelessWidget {
  const _Controls(this.player);

  final LycheePlayer player;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<LycheePlayerValue>(
      valueListenable: player,
      builder: (context, value, _) {
        final duration = value.duration ?? Duration.zero;
        final max = duration.inMilliseconds.toDouble();
        final position = value.position.inMilliseconds.toDouble().clamp(
              0.0,
              max,
            );
        final playing = value.playing && value.state != LycheePlayerState.ended;
        return Padding(
          padding: const EdgeInsets.fromLTRB(8, 0, 8, 8),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  IconButton(
                    icon: Icon(playing ? Icons.pause : Icons.play_arrow),
                    onPressed: playing ? player.pause : player.play,
                  ),
                  Text(_format(value.position)),
                  Expanded(
                    child: Slider(
                      value: max > 0 ? position : 0,
                      max: max > 0 ? max : 1,
                      onChanged: max > 0
                          ? (v) =>
                              player.seek(Duration(milliseconds: v.round()))
                          : null,
                    ),
                  ),
                  Text(_format(duration)),
                ],
              ),
              Row(
                children: [
                  TextButton(
                    onPressed: () => player.setRate(_nextRate(value.rate)),
                    child: Text('${value.rate}x'),
                  ),
                  IconButton(
                    tooltip: 'Loop',
                    icon: Icon(value.looping ? Icons.repeat_on : Icons.repeat),
                    onPressed: () => player.setLooping(!value.looping),
                  ),
                  if (value.tracks.length > 2) _TrackMenu(player, value.tracks),
                  Expanded(
                      child: _Stats(player, value, key: ObjectKey(player))),
                ],
              ),
            ],
          ),
        );
      },
    );
  }

  static double _nextRate(double rate) {
    const rates = [0.5, 1.0, 1.5, 2.0];
    final next = rates.indexWhere((r) => r > rate);
    return next < 0 ? rates.first : rates[next];
  }

  static String _format(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return d.inHours > 0 ? '${d.inHours}:$m:$s' : '$m:$s';
  }
}

/// The subtitle text over the bottom of the video.
class _Subtitle extends StatelessWidget {
  const _Subtitle(this.player);

  final LycheePlayer player;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<LycheePlayerValue>(
      valueListenable: player,
      builder: (context, value, _) {
        final text = value.subtitle;
        if (text == null) {
          return const SizedBox.shrink();
        }
        return Positioned(
          left: 16,
          right: 16,
          bottom: 16,
          child: Center(
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: Colors.black54,
                borderRadius: BorderRadius.circular(4),
              ),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                child: Text(
                  text,
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: Colors.white, fontSize: 18),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Audio and subtitle tracks.
class _TrackMenu extends StatelessWidget {
  const _TrackMenu(this.player, this.tracks);

  final LycheePlayer player;
  final List<LycheeTrack> tracks;

  @override
  Widget build(BuildContext context) {
    final choices =
        tracks.where((t) => t.type != LycheeTrackType.video).toList();
    // Values are track indices; -1 turns subtitles off.
    return PopupMenuButton<int>(
      tooltip: 'Tracks',
      icon: const Icon(Icons.subtitles),
      onSelected: (index) => index < 0
          ? player.selectTrack(LycheeTrackType.subtitle, null)
          : player.selectTrack(tracks[index].type, index),
      itemBuilder: (context) => [
        for (final track in choices)
          CheckedPopupMenuItem(
            value: track.index,
            checked: track.selected,
            child: Text(_label(track)),
          ),
        const PopupMenuItem(value: -1, child: Text('Subtitles off')),
      ],
    );
  }

  static String _label(LycheeTrack track) {
    final name = [
      track.title,
      track.language,
      track.codec,
    ].where((s) => s.isNotEmpty).join(' ');
    return '${track.type.name}: $name';
  }
}

/// The stats line. Samples the counters twice a second; the frame rate is
/// the frames rendered since the previous sample.
class _Stats extends StatefulWidget {
  const _Stats(this.player, this.value, {super.key});

  final LycheePlayer player;
  final LycheePlayerValue value;

  @override
  State<_Stats> createState() => _StatsState();
}

class _StatsState extends State<_Stats> {
  final _clock = Stopwatch()..start();
  late final Timer _timer;
  var _stats = const LycheePlayerStats();
  var _sampledAt = Duration.zero;
  var _fps = 0.0;

  @override
  void initState() {
    super.initState();
    _stats = widget.player.getStats();
    _timer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => setState(_sample),
    );
  }

  void _sample() {
    final stats = widget.player.getStats();
    final now = _clock.elapsed;
    final frames = stats.framesRendered - _stats.framesRendered;
    _fps = frames * 1e6 / (now - _sampledAt).inMicroseconds;
    _stats = stats;
    _sampledAt = now;
  }

  @override
  void dispose() {
    _timer.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final value = widget.value;
    return Text(
      '${_stats.hardwareDecoding ? 'hw' : 'sw'}  '
      '${_fps.toStringAsFixed(1)} fps  '
      'rendered ${_stats.framesRendered}  dropped ${_stats.framesDropped}   '
      '${value.state.name}'
      '${value.error == null ? '' : ': ${value.error}'}',
      maxLines: 2,
      overflow: TextOverflow.ellipsis,
    );
  }
}
