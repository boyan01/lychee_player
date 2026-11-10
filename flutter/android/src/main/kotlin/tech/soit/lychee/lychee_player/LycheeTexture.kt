package tech.soit.lychee.lychee_player

import android.util.Size
import android.view.Surface
import android.app.Activity
import io.flutter.view.TextureRegistry
import tech.soit.lychee.LycheeFrameDriver
import tech.soit.lychee.LycheePlayer

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
 * While Dart shows the player in a [LycheeSurfaceView], frames go into
 * its SurfaceView instead.
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
    // The view frames go into instead, if any, and its rectangle (left,
    // top, width, height in pixels of the FlutterView) while Dart asks
    // for one, also while the Activity is away.
    private var view: LycheeSurfaceView? = null
    private var viewRect: IntArray? = null
    // The size of CPU frames since the driver asked for a CPU Surface; null
    // for MediaCodec.
    private var cpuSize: Size? = null
    private val driver = LycheeFrameDriver(player, refreshRate, false, ::onSurfaceNeeded)

    init {
        driver.setSurface(surface)
    }

    // While the plugin has one.
    private var activity: Activity? = null

    /** Dart's LycheePlayer.setDisplayModeMatching(). */
    var displayModeMatching = false
        set(value) {
            field = value
            applyDisplayModeMatching()
        }

    fun setActivity(activity: Activity?) {
        if (released) return
        removeView()
        this.activity = activity
        applyDisplayModeMatching()
        viewRect?.let(::setViewRect)
    }

    private fun applyDisplayModeMatching() {
        if (!released) driver.setDisplayModeMatching(if (displayModeMatching) activity?.window else null)
    }

    /**
     * Dart's LycheeVideo(surfaceView: true) at [rect] (left, top, width,
     * height in pixels of the FlutterView), or gone with null. False when
     * the Activity cannot show a [LycheeSurfaceView]: Dart then draws the
     * texture.
     */
    fun setViewRect(rect: IntArray?): Boolean {
        if (released) return false
        viewRect = rect
        if (rect == null) {
            removeView()
            return true
        }
        val view = view ?: activity?.let(LycheeSurfaceView::create)?.also {
            it.setRect(rect[0], rect[1], rect[2], rect[3])
            attachView(it)
        } ?: return false
        view.setRect(rect[0], rect[1], rect[2], rect[3])
        return true
    }

    private fun removeView() {
        val view = view ?: return
        detachView(view)
        view.remove()
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
            if (!released && this.view === view) {
                driver.setSurface(surface)
                if (surface != null && cpuSize == null) {
                    // On some MediaTek TVs, switching from the texture leaves
                    // video GPU-composited. Recreate with this view attached
                    // to recover native video composition.
                    LycheePlayer.rebuildVideoDecoderHandle(player)
                }
            }
        }
    }

    private fun attachView(view: LycheeSurfaceView) {
        this.view = view
        newViewSurface(view)
    }

    // Back to the texture.
    private fun detachView(view: LycheeSurfaceView) {
        if (this.view !== view) return
        this.view = null
        driver.setSurface(null)
        // The SurfaceTexture's old Surface may have been connected to
        // MediaCodec before; a new one connects to either.
        newTextureSurface()
    }

    fun release() {
        view?.remove()
        view = null
        released = true
        // The player may be destroyed as soon as this returns.
        driver.close()
        surface.release()
        entry.release()
    }
}
