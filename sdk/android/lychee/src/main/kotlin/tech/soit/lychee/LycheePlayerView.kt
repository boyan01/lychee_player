package tech.soit.lychee

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.Size
import android.view.PixelCopy
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout

/**
 * Shows a player's video in a [SurfaceView], aspect-fit, with its subtitle
 * pictures over it (text subtitles are left to the caller:
 * [LycheePlayer.subtitle]). The player renders it upright (see
 * [LycheePlayer]). Frames are pulled only while the view is attached to a
 * window. Main thread only.
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
    private val subtitleView = SubtitleView(context)
    private val listener = LycheePlayer.Listener { event ->
        if (event is LycheePlayer.Event.VideoSizeChanged || event == LycheePlayer.Event.MediaInfo) {
            requestLayout()
        }
        if (event is LycheePlayer.Event.SubtitleChanged) {
            showSubtitle(if (event.pictures) player else null)
        }
    }

    init {
        setBackgroundColor(Color.BLACK)
        addView(subtitleView)
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
        showSubtitle(null)
        player?.removeListener(listener)
        driver?.close()
        driver = null
        surfaceView?.let(::removeView)
        surfaceView = null
    }

    /**
     * Copies the video on screen at the view's size, as the compositor
     * shows it, with the presentation time of its frame (-1 if unknown).
     * The time is exact only while the player is paused and the display
     * has caught up; for checking frames by pixels (tool/frame_check.py).
     * [onResult] gets null if there is nothing to copy.
     */
    fun snapshot(onResult: (bitmap: Bitmap?, ptsUs: Long) -> Unit) {
        val view = surfaceView
        val pts = driver?.framePtsUs ?: -1
        if (view == null || view.width == 0 || view.height == 0 || !view.holder.surface.isValid) {
            onResult(null, pts)
            return
        }
        val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
        PixelCopy.request(view, bitmap, { result ->
            onResult(if (result == PixelCopy.SUCCESS) bitmap else null, pts)
        }, Handler(Looper.getMainLooper()))
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
        // Under the subtitles.
        addView(view, 0)
    }

    private fun showSubtitle(player: LycheePlayer?) {
        val canvas = player?.let(::uprightSize)
        val subtitle = canvas?.let { player.subtitlePictures(it.width, it.height) }
        if (subtitle != null && subtitle.id == subtitleView.subtitleId) return
        subtitleView.show(subtitle, canvas)
    }

    private fun uprightSize(player: LycheePlayer): Size? {
        val size = player.videoSize ?: return null
        if (size.width <= 0 || size.height <= 0) return null
        return if (player.rotation % 180 != 0) Size(size.height, size.width) else size
    }

    // Draws the pictures scaled from the canvas they were laid out on to the
    // view, which covers the video.
    private class SubtitleView(context: Context) : View(context) {
        var subtitleId = 0L
            private set
        private var pictures: List<Pair<Bitmap, Rect>> = emptyList()
        private var canvasSize: Size? = null
        private val paint = Paint(Paint.FILTER_BITMAP_FLAG)

        fun show(subtitle: LycheePlayer.SubtitlePictures?, canvas: Size?) {
            pictures.forEach { it.first.recycle() }
            subtitleId = subtitle?.id ?: 0L
            pictures = subtitle?.pictures ?: emptyList()
            canvasSize = canvas
            invalidate()
        }

        override fun onDraw(canvas: Canvas) {
            val size = canvasSize ?: return
            canvas.scale(width.toFloat() / size.width, height.toFloat() / size.height)
            pictures.forEach { (bitmap, rect) -> canvas.drawBitmap(bitmap, null, rect, paint) }
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
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
        for (view in listOfNotNull(surfaceView, subtitleView)) {
            view.measure(
                MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
            )
        }
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        for (view in listOfNotNull(surfaceView, subtitleView)) {
            val x = (right - left - view.measuredWidth) / 2
            val y = (bottom - top - view.measuredHeight) / 2
            view.layout(x, y, x + view.measuredWidth, y + view.measuredHeight)
        }
    }
}
