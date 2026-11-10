package tech.soit.lychee.lychee_player

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
        if (cpu) entry.surfaceTexture().setDefaultBufferSize(width, height)
        val old = surface
        surface = Surface(entry.surfaceTexture())
        driver.setSurface(surface)
        old.release()
    }

    fun release() {
        released = true
        // The player may be destroyed as soon as this returns.
        driver.close()
        surface.release()
        entry.release()
    }
}
