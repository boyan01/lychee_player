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
      text: const String.fromEnvironment('LYCHEE_URL'));
  LycheePlayer? _player;

  @override
  void initState() {
    super.initState();
    if (_url.text.isNotEmpty) {
      _open();
    }
  }

  Future<void> _open() async {
    _player?.dispose();
    final player = await LycheePlayer.create();
    if (!mounted) {
      player.dispose();
      return;
    }
    player.open(_url.text.trim());
    player.play();
    setState(() => _player = player);
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
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(8),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _url,
                    decoration: const InputDecoration(
                        hintText: 'File path or URL', isDense: true),
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
                  : Center(child: LycheeVideo(player)),
            ),
          ),
          if (player != null) _Controls(player),
        ],
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
        final position =
            value.position.inMilliseconds.toDouble().clamp(0.0, max);
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8),
          child: Row(
            children: [
              IconButton(
                icon: Icon(value.playing ? Icons.pause : Icons.play_arrow),
                onPressed: value.playing ? player.pause : player.play,
              ),
              Text(_format(value.position)),
              Expanded(
                child: Slider(
                  value: max > 0 ? position : 0,
                  max: max > 0 ? max : 1,
                  onChanged: max > 0
                      ? (v) => player.seek(Duration(milliseconds: v.round()))
                      : null,
                ),
              ),
              Text(_format(duration)),
              const SizedBox(width: 12),
              Text(value.error ?? value.state.name),
            ],
          ),
        );
      },
    );
  }

  static String _format(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return d.inHours > 0 ? '${d.inHours}:$m:$s' : '$m:$s';
  }
}
