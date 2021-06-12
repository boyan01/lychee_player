import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';

import 'audio_player_common.dart';
import 'audio_player_platform.dart';
import 'ffi_player.dart';

class VideoView extends StatefulWidget {
  final FfiAudioPlayer player;

  const VideoView({Key? key, required AudioPlayer player})
      : player = player as FfiAudioPlayer,
        super(key: key);

  @override
  _VideoViewState createState() => _VideoViewState();
}

class _VideoViewState extends State<VideoView> {
  int? _textureId;

  FfiAudioPlayer? _player;

  @override
  void initState() {
    super.initState();
    _initPlayerTextures(widget.player);
  }

  @override
  void didUpdateWidget(covariant VideoView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.player != widget.player) {
      _initPlayerTextures(widget.player);
    }
  }

  void _onPlayerStatusChanged() {
    final player = _player;
    if (player == null) {
      assert(false);
      return;
    }
    if (player.status == PlayerStatus.Idle) {
      // player texture is unavaliable. we should not to use it.
      setState(() {
        player.detachVideoRender();
        _textureId = null;
      });
    } else if (_textureId == null) {
      setState(() {
        _textureId = player.attachVideoRender();
      });
    }
  }

  void _initPlayerTextures(FfiAudioPlayer player) {
    if (player == _player) {
      return;
    }
    _player?.detachVideoRender();
    _textureId = null;
    _player = player;
    player.onStatusChanged.addListener(_onPlayerStatusChanged);
    _onPlayerStatusChanged();
  }

  @override
  void dispose() {
    _player?.detachVideoRender();
    _player?.onStatusChanged.removeListener(_onPlayerStatusChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
        color: Colors.white,
        child: _VideoTexture(
          textureId: _textureId,
        ));
  }
}

class _VideoTexture extends StatelessWidget {
  const _VideoTexture({
    Key? key,
    this.textureId,
  }) : super(key: key);

  final int? textureId;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: AspectRatio(
        // aspectRatio: widget.player.aspectRatio.value,
        aspectRatio: 16.0 / 9.0,
        child: Container(
          color: Colors.black,
          child: textureId == null ? null : Texture(textureId: textureId!),
        ),
      ),
    );
  }
}
