import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';

import 'audio_player_platform.dart';
import 'ffi_player.dart';

class VideoView extends StatefulWidget {
  final FfiAudioPlayer player;

  const VideoView({Key? key, required AudioPlayer player})
      : this.player = player as FfiAudioPlayer,
        super(key: key);

  @override
  _VideoViewState createState() => _VideoViewState();
}

class _VideoViewState extends State<VideoView> {
  int _textureId = -1;

  @override
  void initState() {
    super.initState();
    _textureId = widget.player.attachVideoRender();
  }

  @override
  void didUpdateWidget(covariant VideoView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.player != widget.player) {
      if (_textureId > 0) {
        oldWidget.player.detachVideoRender();
      }
      _textureId = widget.player.attachVideoRender();
    }
  }

  @override
  void dispose() {
    widget.player.detachVideoRender();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    Widget? texture;
    if (_textureId >= 0 /* Android Start from 0 */) {
      texture = AnimatedBuilder(
        animation: widget.player.aspectRatio,
        builder: (context, child) {
          final aspectRatio = widget.player.aspectRatio.value;
          debugPrint("aspectRatio = ${aspectRatio}");
          if (aspectRatio.isInfinite || aspectRatio <= 0) {
            return Center(child: child);
          }
          return Center(
            child: AspectRatio(
              aspectRatio: widget.player.aspectRatio.value,
              child: child,
            ),
          );
        },
        child: Texture(textureId: _textureId),
      );
    }
    return Container(color: Colors.black, child: texture);
  }
}
