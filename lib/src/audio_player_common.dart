import 'package:flutter/foundation.dart';
import 'media_player.dart';

enum PlayerStatus {
  Idle,
  Buffering,
  Ready,
  End,
}

class DurationRange {
  final Duration start;
  final Duration end;

  DurationRange(this.start, this.end);

  factory DurationRange.mills(int start, int end) {
    return DurationRange(
      Duration(milliseconds: start),
      Duration(milliseconds: end),
    );
  }

  double startFraction(Duration duration) {
    return start.inMilliseconds / duration.inMilliseconds;
  }

  double endFraction(Duration duration) {
    return end.inMilliseconds / duration.inMilliseconds;
  }

  @override
  String toString() {
    return '$runtimeType{start: $start, end: $end}';
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is DurationRange &&
          runtimeType == other.runtimeType &&
          start == other.start &&
          end == other.end;

  @override
  int get hashCode => start.hashCode ^ end.hashCode;
}

extension DurationRangeList on List<DurationRange> {
  Duration get max {
    Duration max = Duration.zero;
    for (final element in this) {
      if (element.end > max) {
        max = element.end;
      }
    }
    return max;
  }
}

abstract class AudioPlayerDisposable {
  factory AudioPlayerDisposable.empty() => const _EmptyAudioPlayerDisposable();

  void dispose();
}

class _EmptyAudioPlayerDisposable implements AudioPlayerDisposable {
  const _EmptyAudioPlayerDisposable();

  @override
  void dispose() {}
}

extension AudioPlayerCommon on AudioPlayer {
  AudioPlayerDisposable onReady(VoidCallback action) {
    if (status == PlayerStatus.Ready) {
      action();
      return AudioPlayerDisposable.empty();
    } else {
      return _AudioPlayerReadyListener(onStatusChanged, action);
    }
  }
}

class _AudioPlayerReadyListener implements AudioPlayerDisposable {
  final VoidCallback callback;
  final ValueListenable<PlayerStatus> status;

  _AudioPlayerReadyListener(this.status, this.callback) {
    status.addListener(_onChanged);
  }

  void _onChanged() {
    if (status.value == PlayerStatus.Ready) {
      callback();
    }
  }

  @override
  void dispose() {
    status.removeListener(_onChanged);
  }
}
