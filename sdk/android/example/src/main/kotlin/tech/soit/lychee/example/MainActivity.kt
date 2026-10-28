// Plays the "url" extra (default: h264_aac.mp4 from tool/serve_media.py,
// through `adb reverse tcp:8765 tcp:8765`) on a loop, with a stats line
// under the video. The "hw" extra (default true) turns hardware decoding
// off. Starting it again while it runs (am start without -S) opens the new
// "url" on the same player: a file switch. With "fresh" (true), on a new
// player instead, closing the old one a second later, as apps that make a
// player per file do: two hardware decoders are open for that second.

package tech.soit.lychee.example

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.LinearLayout
import android.widget.TextView
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
