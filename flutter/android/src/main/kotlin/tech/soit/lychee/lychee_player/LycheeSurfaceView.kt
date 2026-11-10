package tech.soit.lychee.lychee_player

import android.content.Context
import android.graphics.Color
import android.util.Size
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import io.flutter.plugin.common.StandardMessageCodec
import io.flutter.plugin.platform.PlatformView
import io.flutter.plugin.platform.PlatformViewFactory

/**
 * Dart's LycheeVideo(surfaceView: true): the player's frames go into a
 * SurfaceView shown as a platform view (Hybrid Composition) instead of the
 * texture, while the view exists. The compositor then shows MediaCodec's
 * buffers itself: at the panel's resolution where the UI layer is smaller
 * (4K TVs whose UI runs at 1080p), with HDR passed on, and without Flutter
 * holding any buffer. Dart sizes the view to the video's rectangle, so the
 * SurfaceView fills it.
 */
internal class LycheeSurfaceView(
    context: Context,
    private val texture: LycheeTexture?,
) : PlatformView {
    private val layout = FrameLayout(context).apply {
        setBackgroundColor(Color.BLACK)
        // D-pad focus stays with Flutter.
        isFocusable = false
        descendantFocusability = ViewGroup.FOCUS_BLOCK_DESCENDANTS
    }
    private var surfaceView: SurfaceView? = null

    init {
        texture?.attachView(this)
    }

    override fun getView(): View = layout

    override fun dispose() {
        texture?.detachView(this)
        surfaceView?.let(layout::removeView)
        surfaceView = null
    }

    /**
     * A new SurfaceView has a new Surface, which MediaCodec or the CPU can
     * connect to whichever used the old one; [cpuSize] is the buffer size
     * CPU frames are copied at. [onSurface] gets its Surface, or null when
     * it goes.
     */
    fun newSurfaceView(cpuSize: Size?, onSurface: (android.view.Surface?) -> Unit) {
        surfaceView?.let(layout::removeView)
        val view = SurfaceView(layout.context)
        view.isFocusable = false
        cpuSize?.let { view.holder.setFixedSize(it.width, it.height) }
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (surfaceView === view) onSurface(holder.surface)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (surfaceView === view) onSurface(null)
            }
        })
        surfaceView = view
        layout.addView(
            view,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ),
        )
    }

    class Factory(private val textures: Map<Long, LycheeTexture>) :
        PlatformViewFactory(StandardMessageCodec.INSTANCE) {
        override fun create(context: Context, viewId: Int, args: Any?): PlatformView {
            val id = ((args as? Map<*, *>)?.get("textureId") as? Number)?.toLong()
            return LycheeSurfaceView(context, id?.let(textures::get))
        }
    }

    companion object {
        const val VIEW_TYPE = "lychee_player/surface_view"
    }
}
