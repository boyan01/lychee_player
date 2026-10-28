package tech.soit.lychee

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.view.Choreographer
import android.view.Surface

/**
 * Pulls a player's frames once per vsync into a [Surface]. The Choreographer
 * runs on its own thread so a busy main thread does not delay frames, at
 * display priority: the decode threads inherit their creator's priority,
 * and software decoding can fill every core.
 *
 * MediaCodec renders hardware-decoded frames straight into the Surface, at
 * whatever size it decodes. Software frames are copied in by the CPU, into
 * a Surface of the frame's size. A Surface the CPU drew into cannot take
 * MediaCodec output any more, so when the frames switch between the two, or
 * software frames change size, [onSurfaceNeeded] asks for a new Surface:
 * `cpu` says which kind, `width` and `height` the size a CPU Surface must
 * have (the frame's, rotated when `upright`). Pass the new Surface to
 * [setSurface].
 *
 * `playerHandle` is a lychee_player* ([LycheePlayer.nativeHandle]); it must
 * outlive the driver until [close]. `upright`: rotate software frames as
 * MediaCodec rotates hardware ones for a player created with
 * lychee_config.rotate_surface_frames, as [LycheePlayer] is.
 *
 * Created, used and closed on the main thread; [onSurfaceNeeded] runs there.
 */
class LycheeFrameDriver(
    playerHandle: Long,
    /** Of the display the Surface is on (`Display.getRefreshRate()`). */
    refreshRate: Float,
    upright: Boolean,
    private val onSurfaceNeeded: (cpu: Boolean, width: Int, height: Int) -> Unit,
) : AutoCloseable, Choreographer.FrameCallback {
    private val handle = nativeCreate(playerHandle, upright)
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
    private var closed = false

    // Main thread only: which kind of Surface setSurface() gets.
    private var cpuMode = false

    init {
        vsyncHandler.post {
            choreographer = Choreographer.getInstance()
            choreographer.postFrameCallback(this)
        }
    }

    /**
     * The Surface frames go into, or null to detach it. The first one, before
     * any [onSurfaceNeeded], is for MediaCodec.
     */
    fun setSurface(surface: Surface?) {
        if (closed) return
        if (cpuMode) {
            nativeSetOutput(handle, null)
            nativeSetCpuSurface(handle, surface)
        } else {
            nativeSetCpuSurface(handle, null)
            nativeSetOutput(handle, surface)
        }
    }

    override fun doFrame(frameTimeNanos: Long) {
        if (closed) {
            return
        }
        // frameTimeNanos is this vsync, in System.nanoTime(), which is
        // lychee_now_us()'s clock. What is drawn now reaches the compositor's
        // next frame and is on screen the vsync after that.
        val displayTimeUs = (frameTimeNanos + 2 * periodNanos) / 1000
        val frame = nativeRender(handle, displayTimeUs)
        val size = frame and 0xffffffffffffL
        val width = (size ushr 24).toInt()
        val height = (size and 0xffffffL).toInt()
        when ((frame ushr 48).toInt()) {
            FRAME_ANDROID_SURFACE -> if (mode != Mode.SURFACE) {
                val fromCpu = mode == Mode.CPU
                mode = Mode.SURFACE
                if (fromCpu) requestSurface(false, width, height)
            }
            FRAME_CPU_RGBA -> if (mode != Mode.CPU || size != requestedSize) {
                mode = Mode.CPU
                requestedSize = size
                requestSurface(true, width, height)
            }
        }
        choreographer.postFrameCallback(this)
    }

    // Vsync thread.
    private fun requestSurface(cpu: Boolean, width: Int, height: Int) {
        mainHandler.post {
            if (closed) return@post
            cpuMode = cpu
            // Let go of the old Surface before a new one is made.
            if (cpu) nativeSetOutput(handle, null) else nativeSetCpuSurface(handle, null)
            onSurfaceNeeded(cpu, width, height)
        }
    }

    /** Stops pulling frames; after it returns, the player is not touched again. */
    override fun close() {
        if (closed) return
        closed = true
        nativeDetach(handle)
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

        init {
            // By name first, so the core's JNI_OnLoad keeps the VM for its
            // http client; lychee_jni's dependency alone would not run it.
            System.loadLibrary("lychee")
            System.loadLibrary("lychee_jni")
        }

        @JvmStatic external fun nativeCreate(player: Long, upright: Boolean): Long
        @JvmStatic external fun nativeSetOutput(handle: Long, surface: Surface?)
        @JvmStatic external fun nativeSetCpuSurface(handle: Long, surface: Surface?)
        @JvmStatic external fun nativeRender(handle: Long, displayTimeUs: Long): Long
        @JvmStatic external fun nativeDetach(handle: Long)
        @JvmStatic external fun nativeDestroy(handle: Long)
    }
}
