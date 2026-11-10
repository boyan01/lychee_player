package tech.soit.lychee.lychee_player

import android.app.Activity
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import io.flutter.embedding.android.FlutterTextureView
import io.flutter.embedding.android.FlutterView

/**
 * Dart's LycheeVideo(surfaceView: true): the player's frames go into a
 * SurfaceView below the FlutterView, which shows through where Dart
 * clears its layer to transparent (the video's rectangle). The compositor
 * then shows MediaCodec's buffers itself: at the panel's resolution where
 * the UI layer is smaller (4K TVs whose UI runs at 1080p), with HDR passed
 * on, and without Flutter holding any buffer.
 *
 * Not a platform view: with a Hybrid Composition one, the Choreographer
 * the frame driver pulls on missed 12 to 18 of every 120 vsyncs on a TCL
 * TV, and a third of 4K60's frames never showed. This path uses a
 * TextureView-backed FlutterView (the Activity's `getRenderMode()` returns
 * `RenderMode.texture`), so Flutter's transparent video rectangle is
 * drawn into the window above the native video. flutter-airplay does the
 * same.
 */
internal class LycheeSurfaceView private constructor(
    private val layer: FrameLayout,
    private val flutterView: View,
) {
    private var surfaceView: SurfaceView? = null
    // In pixels of the FlutterView.
    private var left = 0
    private var top = 0
    private var width = 1
    private var height = 1

    fun setRect(left: Int, top: Int, width: Int, height: Int) {
        this.left = left
        this.top = top
        this.width = width.coerceAtLeast(1)
        this.height = height.coerceAtLeast(1)
        surfaceView?.let(::layout)
    }

    private fun layout(view: SurfaceView) {
        val params = view.layoutParams as FrameLayout.LayoutParams
        val l = flutterView.left + left
        val t = flutterView.top + top
        if (params.width == width && params.height == height &&
            params.leftMargin == l && params.topMargin == t
        ) {
            return
        }
        params.width = width
        params.height = height
        params.leftMargin = l
        params.topMargin = t
        view.layoutParams = params
    }

    fun remove() {
        surfaceView?.let(layer::removeView)
        surfaceView = null
        if (layer.childCount == 0) (layer.parent as? ViewGroup)?.removeView(layer)
    }

    /**
     * A new SurfaceView has a new Surface, which MediaCodec or the CPU can
     * connect to whichever used the old one; [cpuSize] is the buffer size
     * CPU frames are copied at. [onSurface] gets its Surface, or null when
     * it goes.
     */
    fun newSurfaceView(cpuSize: Size?, onSurface: (Surface?) -> Unit) {
        surfaceView?.let(layer::removeView)
        val view = SurfaceView(layer.context)
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
        view.layoutParams = FrameLayout.LayoutParams(width, height)
        layout(view)
        if (layer.parent == null) addLayer()
        layer.addView(view)
    }

    // Right below the FlutterView.
    private fun addLayer() {
        val parent = flutterView.parent as? ViewGroup ?: return
        parent.addView(
            layer,
            parent.indexOfChild(flutterView),
            ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )
    }

    companion object {
        /**
         * A view below [activity]'s FlutterView, or null when Flutter does
         * not draw through a TextureView there.
         */
        fun create(activity: Activity): LycheeSurfaceView? {
            val flutterView = find(activity.window.decorView) ?: return null
            if ((0 until flutterView.childCount).none { flutterView.getChildAt(it) is FlutterTextureView }) {
                return null
            }
            val parent = flutterView.parent as? ViewGroup ?: return null
            // One layer per Activity, for all players' views.
            val layer = (0 until parent.childCount).map(parent::getChildAt)
                .firstOrNull { it.tag === LAYER_TAG } as? FrameLayout
                ?: FrameLayout(activity).also {
                    it.tag = LAYER_TAG
                    it.isFocusable = false
                    it.descendantFocusability = ViewGroup.FOCUS_BLOCK_DESCENDANTS
                }
            return LycheeSurfaceView(layer, flutterView)
        }

        private fun find(view: View): FlutterView? {
            if (view is FlutterView) return view
            if (view !is ViewGroup) return null
            for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
            return null
        }

        private val LAYER_TAG = Any()
    }
}
