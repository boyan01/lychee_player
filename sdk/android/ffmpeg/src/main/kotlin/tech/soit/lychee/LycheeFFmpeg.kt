package tech.soit.lychee

/** Optional FFmpeg backend for the matching version of the Lychee Android SDK. */
object LycheeFFmpeg {
    /**
     * Loads FFmpeg for this process. Call before creating the first player,
     * including players created through the C ABI or Flutter. Repeated calls
     * are safe; a successful load stays active for the process lifetime.
     *
     * @throws IllegalStateException if loading fails, the AARs do not match,
     * or a player was created before the first successful load.
     */
    @JvmStatic
    @Synchronized
    fun load() {
        try {
            System.loadLibrary("lychee")
            nativeLoad()
        } catch (e: UnsatisfiedLinkError) {
            throw IllegalStateException(
                "Cannot load FFmpeg; use matching lychee and lychee-ffmpeg AARs for this device ABI",
                e,
            )
        }
    }

    private external fun nativeLoad()
}
