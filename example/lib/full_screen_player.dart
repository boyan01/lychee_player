import 'package:media_player/media_player.dart';
import 'package:flutter/material.dart';

class FullScreenPage extends StatelessWidget {
  final AudioPlayer player;

  const FullScreenPage({Key? key, required this.player}) : super(key: key);

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text('Full Screen')),
      body: _InteractivePlayerView(player: player),
    );
  }
}

class _InteractivePlayerView extends StatefulWidget {
  const _InteractivePlayerView({
    Key? key,
    required this.player,
  }) : super(key: key);

  final AudioPlayer player;

  @override
  _InteractivePlayerViewState createState() => _InteractivePlayerViewState();
}

class _InteractivePlayerViewState extends State<_InteractivePlayerView> {
  bool _tracking = false;

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
        onExit: (event) {
          _tracking = false;
        },
        onEnter: (event) {
          _tracking = true;
        },
        onHover: (event) {
          debugPrint("on event: ${event}");
        },
        child: VideoView(player: widget.player));
  }
}
