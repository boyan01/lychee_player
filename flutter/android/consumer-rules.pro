# The optional backend is loaded reflectively only in FFmpeg builds.
-keep class tech.soit.lychee.LycheeFFmpeg { public static void load(); }
