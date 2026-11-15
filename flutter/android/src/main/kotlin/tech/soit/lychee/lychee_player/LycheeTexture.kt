package tech.soit.lychee.lychee_player

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.util.Size
import android.view.Surface
import io.flutter.view.TextureRegistry
import tech.soit.lychee.LycheeFrameDriver
import tech.soit.lychee.LycheePlayer

/**
 * Pulls a player's frames once per vsync into a Flutter SurfaceTexture by
 * default, or into a native [LycheeSurfaceView] when Dart sets a view rect
 * (`LycheeVideo(surfaceView: true)`).
 *
 * A SurfaceTexture rather than a SurfaceProducer: the producer's ImageReader
 * keeps up to five frames it has shown, which starves decoders with small
 * output pools (such as MediaTek 4K decoders on TVs); a SurfaceTexture keeps
 * only the frame it latched.
 *
 * MediaCodec and CPU RGBA frames both switch Surfaces when toggling between
 * texture and SurfaceView or when CPU frame sizes change. Created and
 * released on the platform thread.
 */
internal class LycheeTexture(
    private val entry: TextureRegistry.SurfaceTextureEntry,
    val player: Long,
    refreshRate: Float,
    private val onState: (Map<String, String>) -> Unit,
) {
    private var released = false
    private var surface = Surface(entry.surfaceTexture())
    private var view: LycheeSurfaceView? = null
    private var viewRect: IntArray? = null
    private var viewClipRect: IntArray? = null
    private var cpuSize: Size? = null
    private val handler = Handler(Looper.getMainLooper())
    private val timeout = Runnable { fail("SurfaceView creation timed out. Return to playback and retry; restart the app if it persists.") }
    private var state = mapOf("status" to "pending")
    private val driver = LycheeFrameDriver(player, refreshRate, true, ::onSurfaceNeeded).also {
        it.onError = { message -> if (viewRect != null) fail(message) }
    }
    private var activity: Activity? = null
    private var active = true

    init {
        driver.setSurface(surface)
    }

    fun presentationState(): Map<String, String> = state

    private fun pending() {
        handler.removeCallbacks(timeout)
        state = mapOf("status" to "pending")
        onState(state)
        if (activity != null && active && viewRect != null) handler.postDelayed(timeout, 5000)
    }

    private fun fail(message: String) {
        if (released) return
        handler.removeCallbacks(timeout)
        driver.setSurface(null)
        state = mapOf("error" to message)
        onState(state)
    }

    var displayModeMatching = false
        set(value) {
            field = value
            applyDisplayModeMatching()
        }

    fun setActivity(activity: Activity?) {
        if (released) return
        // Activity loss is a suspension, not a failed surface or a texture opt-in.
        removeView()
        this.activity = activity
        applyDisplayModeMatching()
        if (activity != null) viewRect?.let { setViewRect(it, viewClipRect) }
    }

    fun setActive(active: Boolean) {
        if (released || this.active == active) return
        this.active = active
        removeView()
        if (active) viewRect?.let { setViewRect(it, viewClipRect) }
    }

    private fun applyDisplayModeMatching() {
        if (!released) driver.setDisplayModeMatching(if (displayModeMatching) activity?.window else null)
    }

    fun setViewRect(rect: IntArray?, clipRect: IntArray? = null): Boolean {
        if (released) return false
        if (rect != null && (rect.size != 4 || rect[2] <= 0 || rect[3] <= 0 ||
                (clipRect != null && (clipRect.size != 4 || clipRect[2] <= 0 || clipRect[3] <= 0)))) {
            fail("SurfaceView has invalid bounds. Restore the video layout and retry.")
            return false
        }
        val wasSurfaceView = viewRect != null
        viewRect = rect
        viewClipRect = clipRect
        if (rect == null) {
            removeView()
            if (wasSurfaceView) newTextureSurface()
            return true
        }
        if (activity == null || !active) {
            pending()
            return true // Reattach when the Activity returns.
        }
        return try {
            val current = view ?: LycheeSurfaceView.create(activity!!)?.also {
                it.setRect(rect[0], rect[1], rect[2], rect[3], clipRect)
                view = it
                newViewSurface(it)
            } ?: run {
                fail("SurfaceView host unavailable. Use a transparent Flutter TextureView and retry.")
                return false
            }
            current.setRect(rect[0], rect[1], rect[2], rect[3], clipRect)
            true
        } catch (error: RuntimeException) {
            fail("SurfaceView could not be created or bound. Restart the app and retry.")
            false
        }
    }

    private fun removeView() {
        handler.removeCallbacks(timeout)
        val old = view ?: return
        view = null // Ignore callbacks from the view being removed.
        driver.setSurface(null)
        old.remove()
        // A later widget/Activity attachment must not inherit a stale error.
        state = mapOf("status" to "pending")
        onState(state)
    }

    private fun onSurfaceNeeded(cpu: Boolean, width: Int, height: Int) {
        if (released) return
        cpuSize = if (cpu) Size(width, height) else null
        val current = view
        if (current != null) {
            try {
                newViewSurface(current)
            } catch (error: RuntimeException) {
                fail("SurfaceView producer change failed. Retry playback.")
            }
            return
        }
        if (viewRect == null) {
            newTextureSurface()
        }
    }

    private fun newTextureSurface() {
        cpuSize?.let { entry.surfaceTexture().setDefaultBufferSize(it.width, it.height) }
        val old = surface
        surface = Surface(entry.surfaceTexture())
        driver.setSurface(surface)
        old.release()
    }

    private fun newViewSurface(view: LycheeSurfaceView) {
        driver.setSurface(null)
        pending()
        view.newSurfaceView(cpuSize) { surface ->
            if (!released && this.view === view) {
                try {
                    if (surface != null && !surface.isValid) {
                        fail("SurfaceView returned an invalid Surface. Retry playback.")
                    } else {
                        driver.setSurface(surface)
                        if (surface == null) {
                            pending()
                        } else {
                            handler.removeCallbacks(timeout)
                            state = mapOf("status" to "ready")
                            onState(state)
                            if (cpuSize == null) {
                                // Initialize the codec on this native window, also on
                                // TVs where setOutputSurface leaves GPU composition.
                                LycheePlayer.rebuildVideoDecoderHandle(player)
                            }
                        }
                    }
                } catch (error: RuntimeException) {
                    fail("SurfaceView binding failed. Restart the app and retry.")
                }
            }
        }
    }

    fun release() {
        if (released) return
        removeView()
        released = true
        // The player may be destroyed as soon as this returns.
        driver.close()
        surface.release()
        entry.release()
    }
}
