// import 'dart:developer';
// import 'dart:isolate';
//
// import 'package:flutter/foundation.dart';
// import 'package:flutter/services.dart';
// import 'package:system_clock/system_clock.dart';
//
// import 'audio_player_common.dart';
// import 'audio_player_io.dart';
//
// class ChannelAudioPlayer implements AudioPlayer {
//   final String _uri;
//   final DataSourceType _type;
//
//   final ValueNotifier<PlayerStatus> _status = ValueNotifier(PlayerStatus.Idle);
//
//   final String _playerId = _AudioPlayerIdGenerater.generatePlayerId();
//
//   int _currentTime = 0;
//   int _currentUpdateUptime = -1;
//
//   Duration _duration = const Duration(microseconds: -1);
//
//   // Changed by client player play/pause event.
//   // Make [currentTime] more precise
//   bool _isClientPlaying = false;
//
//   final ValueNotifier<List<DurationRange>> _buffered = ValueNotifier(const []);
//
//   final ValueNotifier<dynamic> _error = ValueNotifier(null);
//
//   @override
//   ValueListenable<List<DurationRange>> get buffered => _buffered;
//
//   ChannelAudioPlayer.create(this._uri, this._type) {
//     _remotePlayerManager.create(this);
//     _status.addListener(() {
//       if (_status.value == PlayerStatus.Ready &&
//           _pendingPlay &&
//           playWhenReady) {
//         _pendingPlay = false;
//         _remotePlayerManager.play(_playerId);
//       }
//     });
//   }
//
//   @override
//   void seek(Duration duration) {
//     _remotePlayerManager.seek(_playerId, duration);
//   }
//
//   ValueNotifier<bool> _playWhenReady = ValueNotifier(false);
//
//   bool _pendingPlay = false;
//
//   @override
//   set playWhenReady(bool value) {
//     if (_playWhenReady.value == value) {
//       return;
//     }
//     _playWhenReady.value = value;
//     _pendingPlay = false;
//     if (value) {
//       if (status == PlayerStatus.Ready) {
//         _remotePlayerManager.play(_playerId);
//       } else {
//         _pendingPlay = true;
//       }
//     } else {
//       _remotePlayerManager.pause(_playerId);
//     }
//   }
//
//   @override
//   bool get playWhenReady => _playWhenReady.value;
//
//   @override
//   bool get hasError => _error.value != null;
//
//   @override
//   ValueListenable<dynamic> get error => _error;
//
//   @override
//   Duration get currentTime {
//     if (_currentUpdateUptime == -1 || _currentTime < 0) {
//       return Duration.zero;
//     }
//     if (!_isClientPlaying) {
//       return Duration(milliseconds: _currentTime);
//     }
//     final int offset =
//         SystemClock.uptime().inMilliseconds - _currentUpdateUptime;
//     return Duration(milliseconds: _currentTime + offset.atLeast(0));
//   }
//
//   @override
//   Duration get duration {
//     return _duration;
//   }
//
//   @override
//   bool get isPlaying {
//     return playWhenReady && status == PlayerStatus.Ready;
//   }
//
//   @override
//   PlayerStatus get status => _status.value;
//
//   Listenable? _onStateChange;
//
//   @override
//   Listenable get onStateChanged {
//     if (_onStateChange == null) {
//       _onStateChange = Listenable.merge([_status, _playWhenReady]);
//     }
//     return _onStateChange!;
//   }
//
//   @override
//   ValueListenable<PlayerStatus> get onStatusChanged => _status;
//
//   @override
//   void dispose() {
//     _remotePlayerManager.dispose(this);
//   }
//
//   @override
//   bool operator ==(Object other) {
//     if (other is ChannelAudioPlayer) {
//       return other._playerId == _playerId;
//     }
//     return false;
//   }
//
//   @override
//   int get hashCode => _playerId.hashCode;
//
//   @override
//   String toString() {
//     return "AudioPlayer(id = $_playerId, state = $status, playing = $isPlaying, playWhenReady = $playWhenReady, "
//         "duration = $duration, time = (position: $_currentTime, update: $_currentUpdateUptime, computed: $currentTime))";
//   }
//
//   // TODO
//   @override
//   int volume = 100;
// }
//
// final _RemotePlayerManager _remotePlayerManager = _RemotePlayerManager()
//   ..init();
//
// class _RemotePlayerManager {
//   final MethodChannel _channel =
//       MethodChannel("tech.soit.flutter.audio_player");
//
//   final Map<String, ChannelAudioPlayer> players = {};
//
//   _RemotePlayerManager() {
//     _channel.setMethodCallHandler((call) {
//       return _handleMessage(call)
//         ..catchError((e, s) {
//           debugPrint(e.toString());
//           debugPrintStack(stackTrace: s);
//         });
//     });
//   }
//
//   Future<void> init() async {
//     await _channel.invokeMethod("initialize");
//   }
//
//   Future<dynamic> _handleMessage(MethodCall call) async {
//     final String? playerId = call.argument("playerId");
//     if (call.method == "onPlaybackEvent") {
//       final ChannelAudioPlayer player = players[playerId!]!;
//       final int eventId = call.argument("event");
//       assert(eventId >= 0 && eventId < _ClientPlayerEvent.values.length);
//       final _ClientPlayerEvent event = _ClientPlayerEvent.values[eventId];
//       _updatePlayerState(player, event, call);
//     }
//   }
//
//   void _updatePlayerState(
//       ChannelAudioPlayer player, _ClientPlayerEvent event, MethodCall call) {
//     assert(() {
//       if (event != _ClientPlayerEvent.UpdateBufferPosition) {
//         debugPrint("_updatePlayerState($event): "
//             "${player._playerId} args = ${call.arguments}");
//       }
//       return true;
//     }());
//     const playbackEvents = <_ClientPlayerEvent>{
//       // _ClientPlayerEvent.Playing,
//       // _ClientPlayerEvent.Paused,
//       // _ClientPlayerEvent.BufferingStart,
//       // _ClientPlayerEvent.BufferingEnd,
//       // _ClientPlayerEvent.End,
//       _ClientPlayerEvent.OnIsPlayingChanged,
//     };
//     if (playbackEvents.contains(event)) {
//       final int position = call.argument("position");
//       final int updateTime = call.argument("updateTime");
//       player._currentTime = position;
//       player._currentUpdateUptime = updateTime;
//     }
//
//     switch (event) {
//       case _ClientPlayerEvent.Preparing:
//         player._status.value = PlayerStatus.Buffering;
//         break;
//       case _ClientPlayerEvent.Prepared:
//         final int duration = call.argument("duration");
//         if (duration > 0) {
//           player._duration = Duration(milliseconds: duration);
//         }
//         if (player.hasError) {
//           player._error.value = null;
//         }
//         player._status.value = PlayerStatus.Ready;
//         break;
//       case _ClientPlayerEvent.BufferingStart:
//         player._status.value = PlayerStatus.Buffering;
//         break;
//       case _ClientPlayerEvent.BufferingEnd:
//         player._status.value = PlayerStatus.Ready;
//         break;
//       case _ClientPlayerEvent.Error:
//         player._error.value = "";
//         player._status.value = PlayerStatus.Idle;
//         break;
//       case _ClientPlayerEvent.Seeking:
//         break;
//       case _ClientPlayerEvent.SeekFinished:
//         final bool finished = call.argument("finished") ?? true;
//         if (finished) {
//           final int position = call.argument("position");
//           final int updateTime = call.argument("updateTime");
//           player._currentTime = position;
//           player._currentUpdateUptime = updateTime;
//         }
//         break;
//       case _ClientPlayerEvent.End:
//         player._status.value = PlayerStatus.End;
//         player._playWhenReady.value = false;
//         break;
//       case _ClientPlayerEvent.Paused:
//         if (player._status.value == PlayerStatus.Buffering) {
//           player._status.value = PlayerStatus.Ready;
//         }
//         // Maybe paused by some reason.
//         if (player._playWhenReady.value) {
//           player._playWhenReady.value = false;
//         }
//         break;
//       case _ClientPlayerEvent.UpdateBufferPosition:
//         final List<int> ranges = call.argument<List>("ranges")!.cast();
//         final List<DurationRange> buffered = [];
//         for (var index = 0; index < ranges.length; index += 2) {
//           final DurationRange range =
//               DurationRange.mills(ranges[index], ranges[index + 1]);
//           buffered.add(range);
//         }
//         player._buffered.value = buffered;
//         break;
//       case _ClientPlayerEvent.Playing:
//         if (player._status.value == PlayerStatus.End) {
//           player._status.value = PlayerStatus.Ready;
//         }
//         if (!player._playWhenReady.value) {
//           player._playWhenReady.value = true;
//         }
//         break;
//       case _ClientPlayerEvent.OnIsPlayingChanged:
//         final bool isPlaying = call.argument("playing");
//         player._isClientPlaying = isPlaying;
//         break;
//       default:
//         if (player._status.value == PlayerStatus.Buffering) {
//           player._status.value = PlayerStatus.Ready;
//         }
//         break;
//     }
//   }
//
//   Future<void> create(ChannelAudioPlayer player) async {
//     players[player._playerId] = player;
//     await _channel.invokeMethod("create", {
//       "playerId": player._playerId,
//       "url": player._uri,
//       "type": player._type.index,
//     });
//   }
//
//   Future<void> play(String playerId) async {
//     await _channel.invokeMethod("play", {"playerId": playerId});
//   }
//
//   Future<void> pause(String playerId) async {
//     await _channel.invokeMethod("pause", {"playerId": playerId});
//   }
//
//   Future<void> seek(String playerId, Duration duration) async {
//     await _channel.invokeMethod("seek", {
//       "playerId": playerId,
//       "position": duration.inMilliseconds,
//     });
//   }
//
//   Future<void> dispose(ChannelAudioPlayer player) async {
//     players.remove(player._playerId);
//
//     player._currentTime = 0;
//     player._currentUpdateUptime = -1;
//
//     player._status.value = PlayerStatus.Idle;
//     player._playWhenReady.value = false;
//     player._pendingPlay = false;
//     player._duration = const Duration(microseconds: -1);
//
//     await _channel.invokeMethod("dispose", {"playerId": player._playerId});
//   }
// }
//
// enum _ClientPlayerEvent {
//   Paused,
//   Playing,
//   Preparing,
//   Prepared,
//   BufferingStart,
//   BufferingEnd,
//   Error,
//   Seeking,
//   SeekFinished,
//   End,
//   UpdateBufferPosition,
//   OnIsPlayingChanged
// }
//
// extension _MethodCallArg on MethodCall {
//   T? argument<T>(String key) {
//     final dynamic value = (arguments as Map)[key];
//     assert(value == null || value is T);
//     return value as T?;
//   }
// }
//
// extension _AudioPlayerIdGenerater on AudioPlayer {
//   static int _id = 0;
//
//   static String generatePlayerId() {
//     final String? isolateId = Service.getIsolateID(Isolate.current)!;
//     assert(isolateId != null, "isolate id is empty");
//     return "${isolateId}_${_id++}";
//   }
// }
//
// extension _IntClamp on int {
//   int atLeast(int value) {
//     return this < value ? value : this;
//   }
// }
