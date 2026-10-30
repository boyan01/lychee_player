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
// "soak" (seconds) and "soak_url" repeat two seeks and a file switch.
// "seek" ("SEC:TARGET,...") seeks at SEC seconds after the start.
// "seconds" stops the activity after that many seconds and logs summary:.
// "recreate" (seconds) recreates the activity once to check pending captures.

package tech.soit.lychee.example

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import kotlin.random.Random
import tech.soit.lychee.LycheePlayer
import tech.soit.lychee.LycheePlayerView

class MainActivity : Activity() {
    private lateinit var player: LycheePlayer
    private lateinit var video: LycheePlayerView
    private lateinit var status: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val soakRandom = Random(1)
    private var initialUrl = ""
    private var alternateUrl: String? = null
    private var soakEveryMs = 0L
    private var nextSoakMs = 0L
    private var soakActions = 0
    private var soakOnAlternate = false
    private val quitTask = Runnable {
        val stats = player.stats
        Log.i("lychee-example", "summary: state=${player.state} " +
            "decoder=${stats.videoDecoder} " +
            "position=${player.positionMs / 1000.0}s hw=${if (stats.hardwareDecoding) 1 else 0} " +
            "rendered=${stats.framesRendered} dropped=${stats.framesDropped}")
        finishAndRemoveTask()
    }
    private val tick = object : Runnable {
        override fun run() {
            update()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Reflection is only for this harness: the same example also builds
        // without the optional AAR. Apps call LycheeFFmpeg.load() directly.
        val extension = intent.getStringExtra("ffmpeg")
        if (extension != null && extension != "late") {
            loadFFmpeg(extension)
        }
        val url = intent.getStringExtra("url") ?: "http://127.0.0.1:8765/h264_aac.mp4"
        initialUrl = url
        alternateUrl = intent.getStringExtra("soak_url")
        soakEveryMs = ((intent.getStringExtra("soak")?.toDoubleOrNull() ?: 0.0) * 1000).toLong()
        nextSoakMs = SystemClock.elapsedRealtime() + soakEveryMs
        player = LycheePlayer(hardwareDecoding = intent.getBooleanExtra("hw", true))
        if (extension == "twice") loadFFmpeg("load")
        if (extension == "late") {
            try {
                loadFFmpeg("load")
                error("Late FFmpeg load was accepted")
            } catch (e: java.lang.reflect.InvocationTargetException) {
                check(e.cause is IllegalStateException)
                Log.i("lychee-example", "ffmpeg: late load rejected: ${e.cause?.message}")
            }
        }
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
        for (item in intent.getStringExtra("seek")?.split(',').orEmpty()) {
            val at = item.substringBefore(':').toDoubleOrNull() ?: continue
            val target = item.substringAfter(':').toDoubleOrNull() ?: continue
            handler.postDelayed({
                Log.i("lychee-example", "seek -> ${target}s")
                player.seekTo((target * 1000).toLong())
            }, (at * 1000).toLong())
        }
        scheduleSnapshots(intent.getStringExtra("snapshot"))
        if (savedInstanceState == null) {
            intent.getStringExtra("recreate")?.toDoubleOrNull()?.let { seconds ->
                handler.postDelayed({
                    Log.i("lychee-example", "recreate activity")
                    recreate()
                }, (seconds * 1000).toLong())
            }
        }
        intent.getStringExtra("seconds")?.toDoubleOrNull()?.takeIf { it > 0 }?.let { seconds ->
            handler.postDelayed(quitTask, (seconds * 1000).toLong())
        }
    }

    private fun loadFFmpeg(mode: String) {
        val load = Class.forName("tech.soit.lychee.LycheeFFmpeg").getMethod("load")
        if (mode == "missing") {
            try {
                load.invoke(null)
                error("Missing FFmpeg library was accepted")
            } catch (e: java.lang.reflect.InvocationTargetException) {
                check(e.cause is IllegalStateException)
                check(e.cause?.message?.contains("Cannot load") == true)
                Log.i("lychee-example", "ffmpeg: missing library rejected")
            }
            return
        }
        if (mode == "concurrent") {
            val failures = java.util.concurrent.ConcurrentLinkedQueue<Throwable>()
            val threads = List(8) {
                Thread {
                    try { load.invoke(null) } catch (e: Throwable) { failures.add(e) }
                }.apply { start() }
            }
            threads.forEach { it.join() }
            check(failures.isEmpty()) { failures.toString() }
        } else {
            load.invoke(null)
            if (mode == "twice") load.invoke(null)
        }
        Log.i("lychee-example", "ffmpeg: $mode loaded")
    }

    private fun scheduleSnapshots(spec: String?) {
        if (spec == null || player.state == LycheePlayer.State.ERROR) return
        // A cold software codec may still be starting at the first delay.
        // Measure snapshot delays from the first displayed frame so two
        // scheduled snapshots observe playback, not startup or a paused codec.
        if (player.stats.framesRendered == 0L) {
            handler.postDelayed({ scheduleSnapshots(spec) }, 50)
            return
        }
        for (item in spec.split(',')) {
            val seconds = item.substringBefore(':').toDoubleOrNull() ?: continue
            val name = item.substringAfter(':')
            handler.postDelayed({
                player.pause()
                // A few vsyncs for the display to show the last frame queued.
                handler.postDelayed({
                    video.snapshot { bitmap, pts ->
                        if (isDestroyed) return@snapshot
                        // An absolute path can use private files on systems
                        // whose scoped storage blocks adb from external files.
                        val file = if (File(name).isAbsolute) {
                            File(name)
                        } else {
                            File(getExternalFilesDir(null), name)
                        }
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
        handler.removeCallbacksAndMessages(null)
        video.player = null
        player.close()
        super.onDestroy()
    }

    private fun update() {
        val now = SystemClock.elapsedRealtime()
        val alternate = alternateUrl
        if (soakEveryMs > 0 && alternate != null && now >= nextSoakMs) {
            nextSoakMs = now + soakEveryMs
            if (soakActions++ % 3 == 2) {
                soakOnAlternate = !soakOnAlternate
                val url = if (soakOnAlternate) alternate else initialUrl
                Log.i("lychee-example", "soak: open -> $url")
                player.open(url)
                player.play()
            } else {
                player.durationMs?.takeIf { it > 0 }?.let { duration ->
                    val target = soakRandom.nextLong(duration)
                    Log.i("lychee-example", "soak: seek -> ${target / 1000.0}s")
                    player.seekTo(target)
                }
            }
        }
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
