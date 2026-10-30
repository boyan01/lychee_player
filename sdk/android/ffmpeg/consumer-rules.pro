# The core resolves this optional entry point by its JNI name.
-keep class tech.soit.lychee.LycheeFFmpeg {
    public static void load();
    native <methods>;
}
