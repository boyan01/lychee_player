package tech.soit.lychee.lychee_player

import android.view.Window
import io.flutter.view.TextureRegistry
import tech.soit.lychee.LycheeFrameDriver

/**
 * Pulls a player's frames once per vsync into a SurfaceProducer, through the
 * Android SDK's frame driver. Flutter rotates the texture widget, so frames
 * stay unrotated (the player is created without rotate_surface_frames).
 *
 * MediaCodec renders at whatever size it decodes, so the producer keeps its
 * size; software frames need a Surface of the frame's size. Either kind of
 * switch takes a new Surface from the producer.
 *
 * Created and released on the platform thread.
 */
internal class LycheeTexture(
    private val producer: TextureRegistry.SurfaceProducer,
    /** The player shown; Dart owns it. */
    val player: Long,
    refreshRate: Float,
) {
    private var released = false
    private val driver = LycheeFrameDriver(player, refreshRate, false, ::onSurfaceNeeded)

    init {
        producer.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
            override fun onSurfaceAvailable() {
                if (!released) driver.setSurface(producer.surface)
            }

            override fun onSurfaceCleanup() {
                if (!released) driver.setSurface(null)
            }
        })
        driver.setSurface(producer.surface)
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
        if (cpu) producer.setSize(width, height)
        driver.setSurface(producer.forcedNewSurface)
    }

    fun release() {
        released = true
        // The player may be destroyed as soon as this returns.
        driver.close()
        producer.release()
    }
}
