package tech.soit.lychee

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.Choreographer
import android.view.Display
import android.view.Surface
import android.view.Window
import kotlin.math.abs
import kotlin.math.roundToInt

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
 * With [setDisplayModeMatching], the display's refresh rate follows the
 * video's frame rate while it plays.
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

    // Display mode matching. Main thread: the window, and its own
    // preferredDisplayModeId from before the first change (null: none
    // made). Vsync thread: what the player said last.
    private var matchWindow: Window? = null
    private var originalModeId: Int? = null
    @Volatile
    private var matching = false
    private var vsyncs = 0
    private val videoInfo = LongArray(3)
    private var lastInfo: Triple<Int, Double, Int>? = null

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
        if (matching && vsyncs++ % MATCH_EVERY_VSYNCS == 0) pollVideoInfo()
        choreographer.postFrameCallback(this)
    }

    /**
     * Display mode matching, off until a window is given: while the video
     * plays, [window] asks its display for the mode at the current
     * resolution whose refresh rate is the smallest multiple of the
     * video's frame rate (`WindowManager.LayoutParams.preferredDisplayModeId`),
     * among modes that support the video's HDR format if it has one and
     * some do (API 34+). The window's own preference comes back when
     * the player stops ([LycheePlayer.State.IDLE] or ERROR), on null,
     * and on [close]. Without a mode at a multiple nothing changes. A
     * TV may blank for a moment while it switches.
     */
    fun setDisplayModeMatching(window: Window?) {
        if (closed || window === matchWindow) return
        restoreDisplayMode()
        matchWindow = window
        matching = window != null
        if (window != null) {
            vsyncHandler.post {
                lastInfo = null
                vsyncs = 0
            }
        }
    }

    // Vsync thread.
    private fun pollVideoInfo() {
        if (!nativeVideoInfo(handle, videoInfo)) return
        val state = videoInfo[0].toInt()
        val active = state != STATE_IDLE && state != STATE_ERROR
        val fps = if (active) Double.fromBits(videoInfo[1]) else 0.0
        val transfer = if (active) videoInfo[2].toInt() else 0
        val info = Triple(if (active) 1 else 0, fps, transfer)
        val last = lastInfo
        // Measured rates settle over a few frames; a request each time
        // could make a TV switch more than once.
        if (last != null && last.first == info.first && last.third == info.third &&
            (last.second == fps || (last.second > 0 && abs(fps / last.second - 1) < 5e-4))
        ) {
            return
        }
        lastInfo = info
        mainHandler.post {
            if (closed || !matching) return@post
            if (fps > 0) matchDisplayMode(fps, transfer) else restoreDisplayMode()
        }
    }

    // Main thread.
    private fun matchDisplayMode(fps: Double, transfer: Int) {
        val window = matchWindow ?: return
        val display = window.decorView.display ?: return
        val current = display.mode
        val hdrType = when (transfer) {
            16 -> Display.HdrCapabilities.HDR_TYPE_HDR10
            18 -> Display.HdrCapabilities.HDR_TYPE_HLG
            else -> null
        }
        fun multiple(mode: Display.Mode): Int? {
            val n = (mode.refreshRate / fps).roundToInt()
            return n.takeIf { it >= 1 && abs(mode.refreshRate - n * fps) <= mode.refreshRate * 1e-3 }
        }
        var candidates = display.supportedModes.filter {
            it.physicalWidth == current.physicalWidth &&
                it.physicalHeight == current.physicalHeight && multiple(it) != null
        }
        if (hdrType != null && Build.VERSION.SDK_INT >= 34) {
            candidates.filter { hdrType in it.supportedHdrTypes }.takeIf { it.isNotEmpty() }
                ?.let { candidates = it }
        }
        val mode = candidates.minByOrNull { multiple(it)!! }
        val modes = display.supportedModes.joinToString { "${it.modeId}:${it.physicalWidth}x${it.physicalHeight}@${it.refreshRate}" }
        val video = "%.3f fps".format(fps) + if (hdrType != null) " HDR (transfer $transfer)" else ""
        if (mode == null) {
            Log.i(TAG, "display mode matching: $video: no mode at a multiple of it (modes: $modes)")
            restoreDisplayMode()
            return
        }
        val attributes = window.attributes
        if (originalModeId == null) originalModeId = attributes.preferredDisplayModeId
        Log.i(TAG, "display mode matching: $video: requesting mode ${mode.modeId} " +
            "${mode.physicalWidth}x${mode.physicalHeight}@${mode.refreshRate} " +
            "(current ${current.modeId}; modes: $modes)")
        if (attributes.preferredDisplayModeId != mode.modeId) {
            attributes.preferredDisplayModeId = mode.modeId
            window.attributes = attributes
        }
    }

    // Main thread.
    private fun restoreDisplayMode() {
        val window = matchWindow ?: return
        val original = originalModeId ?: return
        originalModeId = null
        Log.i(TAG, "display mode matching: restoring preferred mode $original")
        val attributes = window.attributes
        attributes.preferredDisplayModeId = original
        window.attributes = attributes
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

    /**
     * The presentation time of the last frame put into the Surface, in
     * microseconds on the media's timeline, or -1 before the first. Paused,
     * it is the frame on screen once the display has caught up (a few
     * vsyncs).
     */
    val framePtsUs: Long
        get() = if (closed) -1 else nativeFramePts(handle)

    /** Stops pulling frames; after it returns, the player is not touched again. */
    override fun close() {
        if (closed) return
        restoreDisplayMode()
        matching = false
        matchWindow = null
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
        // lychee_state.
        const val STATE_IDLE = 0
        const val STATE_ERROR = 5
        // About twice a second at 60 Hz.
        const val MATCH_EVERY_VSYNCS = 30
        const val TAG = "lychee"

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
        @JvmStatic external fun nativeFramePts(handle: Long): Long
        @JvmStatic external fun nativeVideoInfo(handle: Long, longs: LongArray): Boolean
        @JvmStatic external fun nativeDetach(handle: Long)
        @JvmStatic external fun nativeDestroy(handle: Long)
    }
}
