package tech.soit.lychee.lychee_player

import android.util.Size
import android.view.Surface
import android.view.Window
import io.flutter.view.TextureRegistry
import tech.soit.lychee.LycheeFrameDriver

/**
 * Pulls a player's frames once per vsync into a Flutter SurfaceTexture,
 * through the Android SDK's frame driver. Flutter rotates the texture
 * widget, so frames stay unrotated (the player is created without
 * rotate_surface_frames).
 *
 * A SurfaceTexture rather than a SurfaceProducer: the producer's
 * ImageReader keeps up to five frames it has shown (two queued, three for
 * frames in flight), and some hardware decoders allocate barely more output
 * buffers than they need for reference frames (MediaTek's 4K decoders on
 * TCL TVs: about ten, five or six of them references). With four or more
 * held by Flutter such a decoder runs out and stops; a SurfaceTexture keeps
 * only the frame it latched.
 *
 * MediaCodec renders at whatever size it decodes; software frames need a
 * Surface of the frame's size. Either kind of switch takes a new Surface.
 * While a [LycheeSurfaceView] shows the player, frames go into its
 * SurfaceView instead.
 *
 * Created and released on the platform thread.
 */
internal class LycheeTexture(
    private val entry: TextureRegistry.SurfaceTextureEntry,
    /** The player shown; Dart owns it. */
    val player: Long,
    refreshRate: Float,
) {
    private var released = false
    private var surface = Surface(entry.surfaceTexture())
    // The platform view frames go into instead, if any.
    private var view: LycheeSurfaceView? = null
    // The size of CPU frames since the driver asked for a CPU Surface; null
    // for MediaCodec.
    private var cpuSize: Size? = null
    private val driver = LycheeFrameDriver(player, refreshRate, false, ::onSurfaceNeeded)

    init {
        driver.setSurface(surface)
    }

    // The Activity's window, while the plugin has one.
    private var window: Window? = null

    /** Dart's LycheePlayer.setDisplayModeMatching(). */
    var displayModeMatching = false
        set(value) {
            field = value
            applyDisplayModeMatching()
        }

    fun setWindow(window: Window?) {
        this.window = window
        applyDisplayModeMatching()
    }

    private fun applyDisplayModeMatching() {
        if (!released) driver.setDisplayModeMatching(if (displayModeMatching) window else null)
    }

    private fun onSurfaceNeeded(cpu: Boolean, width: Int, height: Int) {
        if (released) return
        cpuSize = if (cpu) Size(width, height) else null
        val view = view
        if (view != null) {
            newViewSurface(view)
            return
        }
        newTextureSurface()
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
        view.newSurfaceView(cpuSize) { surface ->
            if (!released && this.view === view) driver.setSurface(surface)
        }
    }

    /** [view] shows the player from now on, until [detachView]. */
    fun attachView(view: LycheeSurfaceView) {
        if (released) return
        this.view = view
        newViewSurface(view)
    }

    /** Back to the texture, if [view] was the one showing the player. */
    fun detachView(view: LycheeSurfaceView) {
        if (released || this.view !== view) return
        this.view = null
        driver.setSurface(null)
        // The SurfaceTexture's old Surface may have been connected to
        // MediaCodec before; a new one connects to either.
        newTextureSurface()
    }

    fun release() {
        released = true
        // The player may be destroyed as soon as this returns.
        driver.close()
        surface.release()
        entry.release()
    }
}
