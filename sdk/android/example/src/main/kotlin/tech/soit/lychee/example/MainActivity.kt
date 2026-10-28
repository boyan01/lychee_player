// Plays the "url" extra (default: h264_aac.mp4 from tool/serve_media.py,
// through `adb reverse tcp:8765 tcp:8765`) on a loop, with a stats line
// under the video. The "hw" extra (default true) turns hardware decoding
// off. Starting it again while it runs (am start without -S) opens the new
// "url" on the same player: a file switch. With "fresh" (true), on a new
// player instead, closing the old one a second later, as apps that make a
// player per file do: two hardware decoders are open for that second.
// "snapshot" ("SEC:NAME,...") pauses at each SEC seconds after the start,
// writes what the video shows as NAME (a PPM in the app's external files
// directory) and plays on, logging a line tool/frame_check.py reads:
//   snapshot PATH pts=US size=WxH video=0,0,W,H

package tech.soit.lychee.example

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import tech.soit.lychee.LycheePlayer
import tech.soit.lychee.LycheePlayerView

class MainActivity : Activity() {
    private lateinit var player: LycheePlayer
    private lateinit var video: LycheePlayerView
    private lateinit var status: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            update()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val url = intent.getStringExtra("url") ?: "http://127.0.0.1:8765/h264_aac.mp4"
        player = LycheePlayer(hardwareDecoding = intent.getBooleanExtra("hw", true))
        player.isLooping = true
        player.open(url)
        player.play()

        video = LycheePlayerView(this).also { it.player = player }
        status = TextView(this).apply {
            setTextColor(Color.WHITE)
            typeface = Typeface.MONOSPACE
            setPadding(16, 8, 16, 8)
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            fitsSystemWindows = true
            addView(video, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(status)
        })
        scheduleSnapshots(intent.getStringExtra("snapshot"))
    }

    private fun scheduleSnapshots(spec: String?) {
        for (item in spec?.split(',').orEmpty()) {
            val seconds = item.substringBefore(':').toDoubleOrNull() ?: continue
            val name = item.substringAfter(':')
            handler.postDelayed({
                player.pause()
                // A few vsyncs for the display to show the last frame queued.
                handler.postDelayed({
                    video.snapshot { bitmap, pts ->
                        val file = File(getExternalFilesDir(null), name)
                        if (bitmap == null) {
                            Log.i("lychee-example", "snapshot ${file.path} failed")
                        } else {
                            writePpm(bitmap, file)
                            Log.i("lychee-example",
                                "snapshot ${file.path} pts=$pts size=${bitmap.width}x${bitmap.height}" +
                                    " video=0,0,${bitmap.width},${bitmap.height}")
                        }
                        player.play()
                    }
                }, 300)
            }, (seconds * 1000).toLong())
        }
    }

    private fun writePpm(bitmap: Bitmap, file: File) {
        val pixels = IntArray(bitmap.width * bitmap.height)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        val rgb = ByteArray(pixels.size * 3)
        for ((i, pixel) in pixels.withIndex()) {
            rgb[3 * i] = (pixel shr 16).toByte()
            rgb[3 * i + 1] = (pixel shr 8).toByte()
            rgb[3 * i + 2] = pixel.toByte()
        }
        file.outputStream().use {
            it.write("P6\n${bitmap.width} ${bitmap.height}\n255\n".toByteArray())
            it.write(rgb)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        val url = intent.getStringExtra("url") ?: return
        Log.i("lychee-example", "switching to $url")
        if (intent.getBooleanExtra("fresh", false)) {
            val old = player
            player = LycheePlayer(hardwareDecoding = intent.getBooleanExtra("hw", true))
            player.isLooping = true
            video.player = player
            handler.postDelayed({ old.close() }, 1000)
        }
        player.open(url)
        player.play()
    }

    override fun onStart() {
        super.onStart()
        handler.post(tick)
    }

    override fun onStop() {
        handler.removeCallbacks(tick)
        super.onStop()
    }

    override fun onDestroy() {
        video.player = null
        player.close()
        super.onDestroy()
    }

    private fun update() {
        val stats = player.stats
        val size = player.videoSize?.let { "${it.width}x${it.height}" } ?: "-"
        status.text = "${player.state} %.1f/%.1fs %s r%d %s %s rendered=%d dropped=%d".format(
            player.positionMs / 1000.0, (player.durationMs ?: 0) / 1000.0, size,
            player.rotation, if (stats.hardwareDecoding) "hw" else "sw", stats.videoDecoder,
            stats.framesRendered, stats.framesDropped,
        ) + (player.error?.let { " error: $it" } ?: "")
        // For scripts: adb logcat -s lychee-example.
        Log.i("lychee-example", status.text.toString())
    }
}
