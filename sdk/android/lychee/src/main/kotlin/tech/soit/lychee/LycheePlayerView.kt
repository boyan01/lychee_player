package tech.soit.lychee

import android.content.Context
import android.graphics.Color
import android.util.AttributeSet
import android.util.Size
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout

/**
 * Shows a player's video in a [SurfaceView], aspect-fit. The player renders
 * it upright (see [LycheePlayer]). Frames are pulled only while the view is
 * attached to a window. Main thread only.
 */
class LycheePlayerView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {
    /** Set it to null before closing the player. */
    var player: LycheePlayer? = null
        set(value) {
            if (value === field) return
            stop()
            field = value
            start()
            requestLayout()
        }

    private var driver: LycheeFrameDriver? = null
    // Replaced whenever the driver needs a new Surface.
    private var surfaceView: SurfaceView? = null
    private val listener = LycheePlayer.Listener { event ->
        if (event is LycheePlayer.Event.VideoSizeChanged || event == LycheePlayer.Event.MediaInfo) {
            requestLayout()
        }
    }

    init {
        setBackgroundColor(Color.BLACK)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        start()
    }

    override fun onDetachedFromWindow() {
        stop()
        super.onDetachedFromWindow()
    }

    private fun start() {
        val player = player ?: return
        if (!isAttachedToWindow || driver != null) return
        player.addListener(listener)
        driver = LycheeFrameDriver(
            player.nativeHandle, display?.refreshRate ?: 60f, true, ::onSurfaceNeeded,
        )
        newSurfaceView(null)
    }

    private fun stop() {
        player?.removeListener(listener)
        driver?.close()
        driver = null
        surfaceView?.let(::removeView)
        surfaceView = null
    }

    private fun onSurfaceNeeded(cpu: Boolean, width: Int, height: Int) {
        newSurfaceView(if (cpu) Size(width, height) else null)
    }

    // A new SurfaceView has a new Surface, which MediaCodec or the CPU can
    // connect to whichever used the old one. |cpuSize|: the buffer size CPU
    // frames are copied at.
    private fun newSurfaceView(cpuSize: Size?) {
        surfaceView?.let {
            driver?.setSurface(null)
            removeView(it)
        }
        val view = SurfaceView(context)
        cpuSize?.let { view.holder.setFixedSize(it.width, it.height) }
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (surfaceView === view) driver?.setSurface(holder.surface)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (surfaceView === view) driver?.setSurface(null)
            }
        })
        surfaceView = view
        addView(view)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        val view = surfaceView ?: return
        var width = measuredWidth
        var height = measuredHeight
        val player = player
        val size = player?.videoSize
        if (size != null && size.width > 0 && size.height > 0) {
            // Upright: the rotated size.
            val sideways = player.rotation % 180 != 0
            val videoWidth = (if (sideways) size.height else size.width).toLong()
            val videoHeight = (if (sideways) size.width else size.height).toLong()
            if (width * videoHeight > height * videoWidth) {
                width = (height * videoWidth / videoHeight).toInt()
            } else {
                height = (width * videoHeight / videoWidth).toInt()
            }
        }
        view.measure(
            MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
        )
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        val view = surfaceView ?: return
        val x = (right - left - view.measuredWidth) / 2
        val y = (bottom - top - view.measuredHeight) / 2
        view.layout(x, y, x + view.measuredWidth, y + view.measuredHeight)
    }
}
