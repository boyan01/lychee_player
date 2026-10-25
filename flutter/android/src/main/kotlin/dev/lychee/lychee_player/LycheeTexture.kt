package dev.lychee.lychee_player

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.view.Choreographer
import io.flutter.view.TextureRegistry

/**
 * Pulls a player's frames once per vsync into a SurfaceProducer. The
 * Choreographer runs on its own thread so a busy platform thread does not
 * delay frames, at display priority: the decode threads inherit the
 * platform thread's, and software decoding can fill every core.
 *
 * MediaCodec renders hardware-decoded frames into the producer's Surface
 * at whatever size it decodes, so the producer keeps its size. Software
 * frames are copied in by the CPU, into a Surface of the frame's size; a
 * Surface the CPU drew into cannot take MediaCodec output any more, so
 * switching between the two takes a new one.
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
    private var mode = Mode.NONE

    private enum class Mode { NONE, SURFACE, CPU }

    @Volatile
    private var released = false

    // Platform thread only: which Surface a new one from the producer is for.
    private var cpuMode = false

    init {
        producer.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
            override fun onSurfaceAvailable() {
                if (released) return
                if (cpuMode) nativeSetCpuSurface(handle, producer.surface)
                else nativeSetOutput(handle, producer.surface)
            }

            override fun onSurfaceCleanup() {
                if (released) return
                nativeSetOutput(handle, null)
                nativeSetCpuSurface(handle, null)
            }
        })
        nativeSetOutput(handle, producer.surface)
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
        val frame = nativeRender(handle, displayTimeUs)
        when ((frame ushr 48).toInt()) {
            FRAME_ANDROID_SURFACE -> if (mode != Mode.SURFACE) {
                val fromCpu = mode == Mode.CPU
                mode = Mode.SURFACE
                if (fromCpu) {
                    mainHandler.post {
                        if (!released) {
                            cpuMode = false
                            nativeSetCpuSurface(handle, null)
                            nativeSetOutput(handle, producer.forcedNewSurface)
                        }
                    }
                }
            }
            FRAME_CPU_RGBA -> {
                val size = frame and 0xffffffffffffL
                if (mode != Mode.CPU || size != requestedSize) {
                    mode = Mode.CPU
                    requestedSize = size
                    val width = (size ushr 24).toInt()
                    val height = (size and 0xffffffL).toInt()
                    mainHandler.post {
                        if (!released) {
                            cpuMode = true
                            nativeSetOutput(handle, null)
                            producer.setSize(width, height)
                            nativeSetCpuSurface(handle, producer.forcedNewSurface)
                        }
                    }
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
        // lychee_frame_type.
        const val FRAME_CPU_RGBA = 1 shl 2
        const val FRAME_ANDROID_SURFACE = 1 shl 3

        @JvmStatic external fun nativeCreate(player: Long): Long
        @JvmStatic external fun nativeSetOutput(handle: Long, surface: android.view.Surface?)
        @JvmStatic external fun nativeSetCpuSurface(handle: Long, surface: android.view.Surface?)
        @JvmStatic external fun nativeRender(handle: Long, displayTimeUs: Long): Long
        @JvmStatic external fun nativeDetach(handle: Long)
        @JvmStatic external fun nativeDestroy(handle: Long)
    }
}
