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

abstract class AudioPlayerDisposable {
  factory AudioPlayerDisposable.empty() => const _EmptyAudioPlayerDisposable();

  void dispose();
}

class _EmptyAudioPlayerDisposable implements AudioPlayerDisposable {
  const _EmptyAudioPlayerDisposable();

  @override
  void dispose() {}
}
