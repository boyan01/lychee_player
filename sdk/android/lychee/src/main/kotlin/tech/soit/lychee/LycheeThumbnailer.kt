package tech.soit.lychee

import android.graphics.Bitmap
import java.io.Closeable
import java.nio.ByteBuffer

/**
 * Pictures of a file or URL at given positions, for seek bar previews:
 * lychee_thumbnailer, decoded apart from any player with a demuxer and a
 * decoder of its own. The input opens with the first [bitmapAt] and stays
 * open for the next. Times are in milliseconds.
 *
 * Methods can be called from any thread; [bitmapAt] blocks, so call it off
 * the main thread. Calls run one at a time.
 */
class LycheeThumbnailer private constructor(private val handle: Long) : Closeable {
    private val lock = Object()
    private var calls = 0
    private var closed = false

    /**
     * The picture at [positionMs], upright, in ARGB_8888. Without [exact],
     * the keyframe at or before it, which is quick; with it, the frame
     * showing there. Null if the input cannot be opened, has no video,
     * cannot seek there or be decoded (the log says why), or [cancel] or
     * [close] came first.
     */
    fun bitmapAt(positionMs: Long, exact: Boolean = false): Bitmap? {
        synchronized(lock) {
            if (closed) return null
            calls++
        }
        try {
            val size = IntArray(2)
            val pixels = nativeGet(handle, positionMs * 1000, exact, size) ?: return null
            // ARGB_8888 is R, G, B, A bytes, as the core's RGBA.
            val bitmap = Bitmap.createBitmap(size[0], size[1], Bitmap.Config.ARGB_8888)
            bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))
            return bitmap
        } finally {
            synchronized(lock) {
                calls--
                // The last call out destroys what [close] left to it.
                if (closed && calls == 0) nativeDestroy(handle)
            }
        }
    }

    /**
     * Makes the [bitmapAt] calls in progress return null promptly, a
     * stalled network read too. Later calls open the input again.
     */
    fun cancel() = synchronized(lock) {
        if (!closed) nativeCancel(handle)
    }

    /** Cancels the calls in progress; the thumbnailer is gone once they return. */
    override fun close() = synchronized(lock) {
        if (closed) return
        closed = true
        if (calls > 0) nativeCancel(handle) else nativeDestroy(handle)
    }

    companion object {
        init {
            // As LycheePlayer loads them: the core by name first.
            System.loadLibrary("lychee")
            System.loadLibrary("lychee_jni")
        }

        /**
         * Thumbnails of [url], whose http(s) requests carry [headers] as for
         * [LycheePlayer.open], fitted to [maxWidth] x [maxHeight] pixels (0:
         * no limit) keeping their aspect ratio, never enlarged. Opens
         * nothing yet. Null if a header name or value holds a line break.
         */
        @JvmStatic
        @JvmOverloads
        fun create(
            url: String,
            headers: Map<String, String> = emptyMap(),
            maxWidth: Int = 0,
            maxHeight: Int = 0,
        ): LycheeThumbnailer? {
            val lines = LycheePlayer.headerLines(headers) ?: return null
            val handle = nativeCreate(url.toByteArray(), lines.toByteArray(), maxWidth, maxHeight)
            return if (handle == 0L) null else LycheeThumbnailer(handle)
        }

        @JvmStatic private external fun nativeCreate(url: ByteArray, headers: ByteArray, maxWidth: Int, maxHeight: Int): Long
        @JvmStatic private external fun nativeGet(handle: Long, positionUs: Long, exact: Boolean, size: IntArray): ByteArray?
        @JvmStatic private external fun nativeCancel(handle: Long)
        @JvmStatic private external fun nativeDestroy(handle: Long)
    }
}
