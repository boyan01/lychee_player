package dev.lychee.lychee_player

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.view.Choreographer
import io.flutter.view.TextureRegistry

/**
 * Pulls a player's frames once per vsync and draws them into a
 * SurfaceProducer. The Choreographer runs on its own thread so a busy
 * platform thread does not delay frames, at display priority: the decode
 * threads inherit the platform thread's, and software decoding can fill
 * every core.
 *
 * Created and released on the platform thread.
 */
internal class LycheeTexture(
    private val producer: TextureRegistry.SurfaceProducer,
    player: Long,
    refreshRate: Float,
) : Choreographer.FrameCallback {
    private val handle = nativeCreate(player)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val thread =
        HandlerThread("lychee-vsync", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val vsyncHandler = Handler(thread.looper)
    private lateinit var choreographer: Choreographer
    private val periodNanos = (1e9 / refreshRate).toLong()

    // Vsync thread only.
    private var requestedSize = 0L

    @Volatile
    private var released = false

    init {
        producer.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
            override fun onSurfaceAvailable() {
                if (!released) nativeSetSurface(handle, producer.surface)
            }

            override fun onSurfaceDestroyed() {
                if (!released) nativeSetSurface(handle, null)
            }
        })
        nativeSetSurface(handle, producer.surface)
        vsyncHandler.post {
            choreographer = Choreographer.getInstance()
            choreographer.postFrameCallback(this)
        }
    }

    override fun doFrame(frameTimeNanos: Long) {
        if (released) {
            return
        }
        // frameTimeNanos is this vsync, in System.nanoTime(), which is
        // lychee_now_us()'s clock. What is drawn now reaches Flutter's next
        // frame and is on screen the vsync after that.
        val displayTimeUs = (frameTimeNanos + 2 * periodNanos) / 1000
        val size = nativeRender(handle, displayTimeUs)
        if (size != 0L && size != requestedSize) {
            requestedSize = size
            val width = (size ushr 32).toInt()
            val height = size.toInt()
            mainHandler.post {
                if (!released) {
                    producer.setSize(width, height)
                    nativeSetSurface(handle, producer.surface)
                }
            }
        }
        choreographer.postFrameCallback(this)
    }

    fun release() {
        released = true
        // The player may be destroyed as soon as this returns.
        nativeDetach(handle)
        producer.release()
        vsyncHandler.post {
            choreographer.removeFrameCallback(this)
            nativeDestroy(handle)
            thread.quitSafely()
        }
    }

    private companion object {
        @JvmStatic external fun nativeCreate(player: Long): Long
        @JvmStatic external fun nativeSetSurface(handle: Long, surface: android.view.Surface?)
        @JvmStatic external fun nativeRender(handle: Long, displayTimeUs: Long): Long
        @JvmStatic external fun nativeDetach(handle: Long)
        @JvmStatic external fun nativeDestroy(handle: Long)
    }
}
