import 'package:flutter/foundation.dart';

extension ValueNotifierExt<T> on ValueNotifier<T> {
  ValueNotifier<R> map<R>(R Function(T) map) {
    return _MappedValueNotifier(map, this);
  }
}

class _MappedValueNotifier<R, T> extends ValueNotifier<R> {
  final R Function(T) map;
  final ValueNotifier<T> source;

  _MappedValueNotifier(this.map, this.source) : super(map(source.value)) {
    source.addListener(_onSourceChanged);
  }

  void _onSourceChanged() {
    value = map(source.value);
  }

  @override
  void dispose() {
    super.dispose();
    source.removeListener(_onSourceChanged);
  }
}
