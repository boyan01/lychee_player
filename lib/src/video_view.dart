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
        debugPrint("detach texture $_textureId when player idle");
        _textureId = null;
      });
    } else if (_textureId == null) {
      setState(() {
        _textureId = player.attachVideoRender();
        debugPrint('attach video texture: $_textureId');
      });
    }
  }

  void _initPlayerTextures(FfiAudioPlayer player) {
    if (player == _player) {
      return;
    }
    _player?.detachVideoRender();
    _player?.onStateChanged.removeListener(_onPlayerStatusChanged);
    setState(() {
      _textureId = null;
      _player = player;
    });
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
        child: Stack(
          children: [
            _VideoTexture(
              textureId: _textureId,
            ),
            Align(
              alignment: Alignment.topRight,
              child: Container(
                  color: Colors.black,
                  padding: const EdgeInsets.all(4),
                  child: Text(
                    "$_textureId",
                    style: const TextStyle(
                      color: Colors.white,
                    ),
                  )),
            ),
          ],
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
          child: textureId == null
              ? const Text('frame not availible')
              : Texture(
                  textureId: textureId!,
                  key: ValueKey(textureId),
                ),
        ),
      ),
    );
  }
}
