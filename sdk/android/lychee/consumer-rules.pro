# Called from native code by name.
-keep class tech.soit.lychee.LycheePlayer$EventSink {
    void onEvent(int, long, long);
}
