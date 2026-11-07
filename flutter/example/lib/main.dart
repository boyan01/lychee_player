import 'dart:async';
import 'dart:io';
import 'dart:ui' show AppExitType;

import 'package:desktop_drop/desktop_drop.dart';
import 'package:file_selector/file_selector.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:lychee_player/lychee_player.dart';

/// Desktop command line: `[--seconds N] [--subtitle URL] [file or URL]`.
/// The file or URL opens at launch (else the LYCHEE_URL define), with the
/// subtitle file added to it. With --seconds the app
/// prints a summary line in examples/glfw's format after N seconds and
/// quits, for unattended runs. It writes to logcat on Android, stderr
/// elsewhere. Mobile apps get no arguments, so the LYCHEE_SECONDS define
/// does the same there.
void main(List<String> args) {
  var url = const String.fromEnvironment('LYCHEE_URL');
  const defined = int.fromEnvironment('LYCHEE_SECONDS');
  double? seconds = defined > 0 ? defined.toDouble() : null;
  String? subtitle;
  for (var i = 0; i < args.length; i++) {
    if (args[i] == '--seconds' && i + 1 < args.length) {
      seconds = double.tryParse(args[++i]);
    } else if (args[i] == '--subtitle' && i + 1 < args.length) {
      subtitle = args[++i];
    } else if (!args[i].startsWith('-')) {
      url = args[i];
    }
  }
  runApp(ExampleApp(url: url, seconds: seconds, subtitle: subtitle));
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key, this.url = '', this.seconds, this.subtitle});

  final String url;
  final double? seconds;
  final String? subtitle;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'lychee_player',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xfff28d73),
          brightness: Brightness.dark,
          surface: const Color(0xff191b20),
        ),
        scaffoldBackgroundColor: const Color(0xff111318),
        inputDecorationTheme: const InputDecorationTheme(
          filled: true,
          border: OutlineInputBorder(),
        ),
      ),
      home: PlayerPage(url: url, seconds: seconds, subtitle: subtitle),
    );
  }
}

class PlayerPage extends StatefulWidget {
  const PlayerPage({super.key, this.url = '', this.seconds, this.subtitle});

  final String url;
  final double? seconds;
  final String? subtitle;

  @override
  State<PlayerPage> createState() => _PlayerPageState();
}

class _PlayerPageState extends State<PlayerPage> {
  late final _url = TextEditingController(text: widget.url);
  final _playerFocus = FocusNode(debugLabel: 'Player shortcuts');
  LycheePlayer? _player;
  String? _source;
  String? _openError;
  var _dragging = false;
  var _openRequest = 0;
  Timer? _quitTimer;
  final _retainedFiles = <Directory>[];

  bool get _desktop =>
      Platform.isMacOS || Platform.isWindows || Platform.isLinux;

  // The runner's host texture test (LYCHEE_HOST_TEXTURE_TEST), shown in a
  // corner while the runner swaps the players behind it.
  int? _hostTexture;

  @override
  void initState() {
    super.initState();
    if (_url.text.isNotEmpty) {
      _open().then((_) {
        final player = _player;
        if (!mounted) return;
        if (widget.subtitle case final subtitle? when player != null) {
          _addSubtitle(context, player, subtitle);
        }
      });
    }
    final seconds = widget.seconds;
    if (seconds != null) {
      _quitTimer = Timer(
        Duration(microseconds: (seconds * 1e6).round()),
        _quit,
      );
    }
    if ((Platform.isLinux || Platform.isWindows) &&
        Platform.environment.containsKey('LYCHEE_HOST_TEXTURE_TEST')) {
      _startHostTexture();
    }
  }

  Future<void> _startHostTexture() async {
    final id = await const MethodChannel('lychee_example/host_texture')
        .invokeMethod<int>('start');
    if (mounted) setState(() => _hostTexture = id);
  }

  Future<void> _quit() async {
    final player = _player;
    if (player != null) {
      final stats = player.getStats();
      final position = player.value.position.inMicroseconds / 1e6;
      final summary =
          'summary: decoder=${stats.videoDecoder} '
          'hw=${stats.hardwareDecoding ? 1 : 0} '
          'rendered=${stats.framesRendered} dropped=${stats.framesDropped} '
          'position=${position.toStringAsFixed(3)} '
          'state=${player.value.state.name}'
          '${player.value.subtitle == null ? '' : ' subtitle=${player.value.subtitle}'}';
      if (Platform.isAndroid) {
        // Android's process stderr does not reach logcat.
        debugPrintSynchronously(summary);
      } else {
        stderr.writeln(summary);
      }
    }
    // Mobile embedders do not implement exitApplication.
    if (Platform.isIOS || Platform.isAndroid) {
      // IOSink writes are queued: exit must not discard the summary.
      await stderr.flush();
      exit(0);
    }
    // As if the window were closed.
    ServicesBinding.instance.exitApplication(AppExitType.required);
  }

  Future<void> _open({int? request}) async {
    final source = _url.text.trim();
    if (source.isEmpty) return;
    request ??= ++_openRequest;
    setState(() => _openError = null);
    try {
      final player = await LycheePlayer.create();
      // A newer request may finish first. Never replace it with an older one.
      if (!mounted || request != _openRequest) {
        player.dispose();
        return;
      }
      player.open(source);
      player.play();
      final previous = _player;
      setState(() {
        _player = player;
        _source = source;
      });
      previous?.dispose();
      _playerFocus.requestFocus();
    } catch (error) {
      if (mounted && request == _openRequest) {
        setState(() => _openError = 'Could not open media: $error');
      }
    }
  }

  Future<void> _drop(DropDetails details) async {
    setState(() => _dragging = false);
    // A drop may carry several representations. Only consume file items.
    final files = details.items.whereType<DropFileItem>().toList();
    if (files.isEmpty) return;
    final request = ++_openRequest;
    try {
      final file = files.first;
      final path = await _localPath(file.file, copy: file.isPromised);
      if (!mounted || request != _openRequest) return;
      _url.text = path;
      if (files.length > 1) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Opening the first dropped file.')),
        );
      }
      await _open(request: request);
    } catch (error) {
      if (mounted && request == _openRequest) {
        setState(() => _openError = 'Could not read dropped file: $error');
      }
    }
  }

  // Provider references and promised files can expire when onDrop returns.
  // Stream them to a local file before returning; never buffer a video in RAM.
  Future<String> _localPath(XFile file, {bool copy = false}) async {
    if (!copy && file is FileSystemXFile) return file.path;
    final directory = await Directory.systemTemp.createTemp('lychee-example-');
    try {
      var name = (await file.name() ?? 'media').split(RegExp(r'[/\\]')).last;
      if (name.isEmpty || name == '.' || name == '..') name = 'media';
      final local = File('${directory.path}/$name');
      await file.openRead().cast<List<int>>().pipe(local.openWrite());
      if (!mounted) throw StateError('Player page closed during file transfer');
      // Keep copied media and subtitles for this page's lifetime: the core
      // reads them asynchronously, including after seeks and loops.
      _retainedFiles.add(directory);
      return local.path;
    } catch (_) {
      await _removeTemporaryDirectory(directory);
      rethrow;
    }
  }

  Future<void> _pick({LycheePlayer? subtitlePlayer}) async {
    try {
      final file = await openFile(
        acceptedTypeGroups: subtitlePlayer == null
            ? const []
            : const [
                XTypeGroup(
                  label: 'Subtitles',
                  extensions: ['srt', 'vtt', 'ass', 'ssa'],
                ),
              ],
      );
      if (file == null || !mounted) return;
      final request = subtitlePlayer == null ? ++_openRequest : null;
      final path = await _localPath(file);
      if (!mounted) return;
      if (subtitlePlayer != null) {
        if (identical(subtitlePlayer, _player)) {
          await _addSubtitle(context, subtitlePlayer, path);
        }
      } else if (request == _openRequest) {
        _url.text = path;
        await _open(request: request);
      }
    } catch (error) {
      if (mounted) {
        setState(() => _openError = 'Could not read selected file: $error');
      }
    }
  }

  Future<void> _removeTemporaryDirectory(Directory directory) async {
    try {
      await directory.delete(recursive: true);
    } on FileSystemException catch (error) {
      // Windows can still hold an open file during asynchronous teardown.
      debugPrint('Could not remove temporary media: $error');
    }
  }

  @override
  void dispose() {
    _quitTimer?.cancel();
    _player?.dispose();
    _url.dispose();
    _playerFocus.dispose();
    for (final directory in _retainedFiles) {
      unawaited(_removeTemporaryDirectory(directory));
    }
    super.dispose();
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    final player = _player;
    if (!_desktop ||
        player == null ||
        (event is! KeyDownEvent && event is! KeyRepeatEvent)) {
      return KeyEventResult.ignored;
    }
    final keyboard = HardwareKeyboard.instance;
    if (keyboard.isControlPressed ||
        keyboard.isAltPressed ||
        keyboard.isMetaPressed ||
        keyboard.isShiftPressed) {
      return KeyEventResult.ignored;
    }
    final focusContext = FocusManager.instance.primaryFocus?.context;
    if (focusContext?.findAncestorWidgetOfExactType<EditableText>() != null) {
      return KeyEventResult.ignored;
    }
    final value = player.value;
    if (event.logicalKey == LogicalKeyboardKey.space && event is KeyDownEvent) {
      if (value.playing && value.state != LycheePlayerState.ended) {
        player.pause();
      } else {
        player.play();
      }
      return KeyEventResult.handled;
    }
    if (event.logicalKey == LogicalKeyboardKey.arrowLeft ||
        event.logicalKey == LogicalKeyboardKey.arrowRight) {
      final duration = value.duration;
      if (!value.seekable || duration == null || duration <= Duration.zero) {
        return KeyEventResult.ignored;
      }
      final delta = event.logicalKey == LogicalKeyboardKey.arrowLeft ? -5 : 5;
      final target = (value.position.inMilliseconds + delta * 1000).clamp(
        0,
        duration.inMilliseconds,
      );
      player.seek(Duration(milliseconds: target));
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  @override
  Widget build(BuildContext context) {
    final player = _player;
    return Focus(
      focusNode: _playerFocus,
      autofocus: true,
      onKeyEvent: _onKey,
      child: Scaffold(
        body: SafeArea(
          child: DropTarget(
            enabled: _desktop,
            acceptedKinds: const {DropItemKind.file},
            onEnter: (_) => setState(() => _dragging = true),
            onLeave: () => setState(() => _dragging = false),
            onDrop: _drop,
            child: Stack(
              fit: StackFit.expand,
              children: [
                Column(
                  children: [
                    _sourceBar(),
                    if (_openError case final error?)
                      Padding(
                        padding: const EdgeInsets.all(12),
                        child: Text(
                          error,
                          style: TextStyle(
                            color: Theme.of(context).colorScheme.error,
                          ),
                        ),
                      ),
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: ClipRRect(
                          borderRadius: BorderRadius.circular(16),
                          child: ColoredBox(
                            color: Colors.black,
                            child: Stack(
                              fit: StackFit.expand,
                              children: [
                                if (player == null)
                                  _EmptyPlayer(desktop: _desktop, onPick: _pick)
                                else
                                  Center(child: _Picture(player)),
                                if (_hostTexture case final id?)
                                  Positioned(
                                    right: 8,
                                    bottom: 8,
                                    width: 320,
                                    height: 180,
                                    child: Texture(textureId: id),
                                  ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    ),
                    if (player != null)
                      // Keep the video visible even in a short desktop window.
                      ConstrainedBox(
                        constraints: BoxConstraints(
                          maxHeight: MediaQuery.sizeOf(context).height * .45,
                        ),
                        child: SingleChildScrollView(
                          child: _Controls(
                            player,
                            _source!,
                            onAddSubtitle: () => _pick(subtitlePlayer: player),
                            key: ObjectKey(player),
                          ),
                        ),
                      )
                    else
                      const Padding(
                        padding: EdgeInsets.all(16),
                        child: Text(
                          'Local files · Network streams · External subtitles',
                        ),
                      ),
                  ],
                ),
                if (_dragging)
                  Positioned.fill(
                    child: IgnorePointer(
                      child: Container(
                        margin: const EdgeInsets.all(8),
                        decoration: BoxDecoration(
                          color: Theme.of(context).colorScheme.primaryContainer
                              .withValues(alpha: .94),
                          borderRadius: BorderRadius.circular(18),
                          border: Border.all(
                            color: Theme.of(context).colorScheme.primary,
                            width: 2,
                          ),
                        ),
                        child: const Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(Icons.file_download_outlined, size: 48),
                              SizedBox(height: 16),
                              Text(
                                'Release to play',
                                style: TextStyle(fontSize: 24),
                              ),
                              SizedBox(height: 8),
                              Text('Open a media file in this window'),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _sourceBar() {
    final field = TextField(
      controller: _url,
      decoration: const InputDecoration(
        hintText: 'File path or URL',
        prefixIcon: Icon(Icons.link),
        isDense: true,
      ),
      onSubmitted: (_) => _open(),
    );
    final buttons = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        IconButton(
          tooltip: 'Choose a file',
          icon: const Icon(Icons.folder_open_outlined),
          onPressed: _pick,
        ),
        const SizedBox(width: 8),
        FilledButton(onPressed: _open, child: const Text('Open')),
      ],
    );
    return Padding(
      padding: const EdgeInsets.all(16),
      child: LayoutBuilder(
        builder: (context, constraints) {
          return Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                'Lychee Player',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const SizedBox(height: 12),
              if (constraints.maxWidth < 440) ...[
                field,
                const SizedBox(height: 8),
                Align(alignment: Alignment.centerRight, child: buttons),
              ] else
                Row(
                  children: [
                    Expanded(child: field),
                    const SizedBox(width: 12),
                    buttons,
                  ],
                ),
            ],
          );
        },
      ),
    );
  }
}

class _EmptyPlayer extends StatelessWidget {
  const _EmptyPlayer({required this.desktop, required this.onPick});

  final bool desktop;
  final VoidCallback onPick;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        return Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (constraints.maxHeight > 220) ...[
                  Icon(
                    Icons.play_circle_outline,
                    size: 64,
                    color: Theme.of(context).colorScheme.primary,
                  ),
                  const SizedBox(height: 20),
                ],
                Text(
                  desktop ? 'Drop a file to play' : 'Choose something to play',
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.headlineSmall,
                ),
                const SizedBox(height: 8),
                const Text(
                  'Video, audio, or a stream URL above.',
                  textAlign: TextAlign.center,
                ),
                const SizedBox(height: 20),
                OutlinedButton.icon(
                  onPressed: onPick,
                  icon: const Icon(Icons.folder_open_outlined),
                  label: const Text('Choose a file'),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}

class _Controls extends StatefulWidget {
  const _Controls(
    this.player,
    this.source, {
    required this.onAddSubtitle,
    super.key,
  });

  final LycheePlayer player;
  final String source;
  final VoidCallback onAddSubtitle;

  @override
  State<_Controls> createState() => _ControlsState();
}

class _ControlsState extends State<_Controls> {
  double? _scrubbing;
  bool _details = false;

  LycheePlayer get player => widget.player;

  void _skip(LycheePlayerValue value, int seconds) {
    final end = value.duration?.inMilliseconds ?? 0;
    final target = (value.position.inMilliseconds + seconds * 1000).clamp(
      0,
      end,
    );
    player.seek(Duration(milliseconds: target));
  }

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<LycheePlayerValue>(
      valueListenable: player,
      builder: (context, value, _) {
        final duration = value.duration ?? Duration.zero;
        final max = duration.inMilliseconds.toDouble();
        final position =
            (_scrubbing ?? value.position.inMilliseconds.toDouble()).clamp(
              0.0,
              max,
            );
        final playing = value.playing && value.state != LycheePlayerState.ended;
        final uri = Uri.tryParse(widget.source);
        final name = uri != null && uri.pathSegments.isNotEmpty
            ? uri.pathSegments.last
            : widget.source.split(RegExp(r'[/\\]')).last;
        final status = switch (value.state) {
          LycheePlayerState.ready => playing ? 'Playing' : 'Paused',
          LycheePlayerState.loading => 'Opening…',
          LycheePlayerState.buffering => 'Buffering…',
          LycheePlayerState.ended => 'Ended',
          LycheePlayerState.error => 'Playback failed',
          LycheePlayerState.idle => 'Ready',
        };
        return Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.titleSmall,
                    ),
                  ),
                  const SizedBox(width: 12),
                  Text(
                    status,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.primary,
                    ),
                  ),
                ],
              ),
              if (value.error case final error?)
                Padding(
                  padding: const EdgeInsets.only(top: 8),
                  child: Text(
                    error,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.error,
                    ),
                  ),
                ),
              Row(
                children: [
                  Text(_format(Duration(milliseconds: position.round()))),
                  Expanded(
                    child: Slider(
                      value: max > 0 ? position : 0,
                      max: max > 0 ? max : 1,
                      onChanged: max > 0 && value.seekable
                          ? (v) => setState(() => _scrubbing = v)
                          : null,
                      onChangeEnd: max > 0 && value.seekable
                          ? (v) {
                              player.seek(Duration(milliseconds: v.round()));
                              setState(() => _scrubbing = null);
                            }
                          : null,
                    ),
                  ),
                  Text(value.duration == null ? '--:--' : _format(duration)),
                ],
              ),
              Wrap(
                spacing: 8,
                crossAxisAlignment: WrapCrossAlignment.center,
                children: [
                  IconButton.filled(
                    tooltip: playing ? 'Pause' : 'Play',
                    icon: Icon(playing ? Icons.pause : Icons.play_arrow),
                    onPressed: playing ? player.pause : player.play,
                  ),
                  IconButton(
                    tooltip: 'Back 10 seconds',
                    icon: const Icon(Icons.replay_10),
                    onPressed: value.seekable && max > 0
                        ? () => _skip(value, -10)
                        : null,
                  ),
                  IconButton(
                    tooltip: 'Forward 10 seconds',
                    icon: const Icon(Icons.forward_10),
                    onPressed: value.seekable && max > 0
                        ? () => _skip(value, 10)
                        : null,
                  ),
                  TextButton(
                    onPressed: () => player.setRate(_nextRate(value.rate)),
                    child: Text('${value.rate}×'),
                  ),
                  IconButton(
                    tooltip: value.looping ? 'Turn loop off' : 'Turn loop on',
                    isSelected: value.looping,
                    icon: const Icon(Icons.repeat),
                    onPressed: () => player.setLooping(!value.looping),
                  ),
                  _TrackMenu(
                    player,
                    value.tracks,
                    onAddSubtitle: widget.onAddSubtitle,
                  ),
                  IconButton(
                    tooltip: 'Playback details',
                    isSelected: _details,
                    icon: const Icon(Icons.info_outline),
                    onPressed: () => setState(() => _details = !_details),
                  ),
                ],
              ),
              if (_details)
                Padding(
                  padding: const EdgeInsets.only(top: 12),
                  child: _Stats(player, value, key: ObjectKey(player)),
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

/// The video, or the cover picture of audio without video.
class _Picture extends StatelessWidget {
  const _Picture(this.player);

  final LycheePlayer player;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<LycheePlayerValue>(
      valueListenable: player,
      builder: (context, value, video) {
        final artwork = value.artwork;
        if (value.videoSize != null) return video!;
        if (artwork != null) {
          return Image.memory(artwork, gaplessPlayback: true);
        }
        return const Icon(Icons.graphic_eq, size: 80, color: Colors.white38);
      },
      child: LycheeVideo(player),
    );
  }
}

/// Adds a subtitle file to what [player] plays, saying so if it is not one.
Future<void> _addSubtitle(
  BuildContext context,
  LycheePlayer player,
  String url,
) async {
  final added = await player.addSubtitle(url);
  if (!added && context.mounted) {
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text('Not added: $url')));
  }
}

/// Audio and subtitle tracks, and adding a subtitle file.
class _TrackMenu extends StatelessWidget {
  const _TrackMenu(this.player, this.tracks, {required this.onAddSubtitle});

  final LycheePlayer player;
  final List<LycheeTrack> tracks;
  final VoidCallback onAddSubtitle;

  @override
  Widget build(BuildContext context) {
    final choices = tracks
        .where((t) => t.type != LycheeTrackType.video)
        .toList();
    // Values are track indices; -1 turns subtitles off, -2 adds a file.
    return PopupMenuButton<int>(
      tooltip: 'Tracks',
      icon: const Icon(Icons.subtitles),
      onSelected: (index) => switch (index) {
        -1 => player.selectTrack(LycheeTrackType.subtitle, null),
        -2 => onAddSubtitle(),
        _ => player.selectTrack(
          tracks.firstWhere((track) => track.index == index).type,
          index,
        ),
      },
      itemBuilder: (context) => [
        for (final track in choices)
          CheckedPopupMenuItem(
            value: track.index,
            checked: track.selected,
            child: Text(_label(track)),
          ),
        const PopupMenuItem(value: -1, child: Text('Subtitles off')),
        const PopupMenuItem(value: -2, child: Text('Add subtitle file…')),
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
      '${_stats.videoDecoder} ${_stats.hardwareDecoding ? 'hw' : 'sw'}  '
      '${_fps.toStringAsFixed(1)} fps  '
      'rendered ${_stats.framesRendered}  dropped ${_stats.framesDropped}   '
      '${value.state.name}'
      '${value.error == null ? '' : ': ${value.error}'}',
      maxLines: 2,
      overflow: TextOverflow.ellipsis,
    );
  }
}
