import 'package:flutter/widgets.dart';

import 'audio_player_platform.dart';
import 'ffi_player.dart';

class VideoRender extends StatefulWidget {
  final FfiAudioPlayer player;

  const VideoRender({Key? key, required AudioPlayer player})
      : this.player = player as FfiAudioPlayer,
        super(key: key);

  @override
  _VideoRenderState createState() => _VideoRenderState();
}

class _VideoRenderState extends State<VideoRender> {
  int _textureId = -1;

  @override
  void initState() {
    super.initState();
    _textureId = widget.player.attachVideoRender();
  }

  @override
  void didUpdateWidget(covariant VideoRender oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.player != widget.player) {
      if (_textureId > 0) {
        oldWidget.player.detachVideoRender();
      }
      _textureId = widget.player.attachVideoRender();
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_textureId < 0) {
      return Container();
    } else {
      return Texture(textureId: _textureId);
    }
  }
}
