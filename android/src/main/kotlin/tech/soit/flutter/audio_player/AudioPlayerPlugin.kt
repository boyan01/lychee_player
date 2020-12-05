package tech.soit.flutter.audio_player

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.annotation.NonNull
import com.google.android.exoplayer2.ExoPlaybackException
import com.google.android.exoplayer2.MediaItem
import com.google.android.exoplayer2.Player
import com.google.android.exoplayer2.SimpleExoPlayer
import com.google.android.exoplayer2.source.ProgressiveMediaSource
import com.google.android.exoplayer2.upstream.DefaultDataSourceFactory
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result
import java.io.File

/** AudioPlayerPlugin */
class AudioPlayerPlugin : FlutterPlugin, MethodCallHandler {

    companion object {
        private const val TAG = "AudioPlayerPlugin"
    }

    private lateinit var channel: MethodChannel
    private lateinit var applicationContext: Context
    private lateinit var flutterAssets: FlutterPlugin.FlutterAssets

    private val players = mutableMapOf<String, ClientAudioPlayer>()

    override fun onAttachedToEngine(
        @NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding
    ) {
        applicationContext = flutterPluginBinding.applicationContext
        flutterAssets = flutterPluginBinding.flutterAssets
        channel = MethodChannel(
            flutterPluginBinding.binaryMessenger,
            "tech.soit.flutter.audio_player"
        )
        channel.setMethodCallHandler(this)
    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
    }

    private fun destroyAllPlayer() {
        players.values.forEach(ClientAudioPlayer::destroy)
        players.clear()
    }

    override fun onMethodCall(@NonNull call: MethodCall, @NonNull result: Result) {
        if (BuildConfig.DEBUG) {
            Log.d(TAG, "onMethodCall: ${call.method}, args = ${call.arguments}")
        }
        if (call.method == "initialize") {
            destroyAllPlayer()
            result.success(null)
            return
        }
        val playerId: String = call.argument("playerId")!!

        fun answerWithPlayer(action: (player: ClientAudioPlayer) -> Unit) {
            val player = players[playerId]
            if (player != null) {
                action(player)
                result.success()
            } else {
                result.errorWithPlayerNotCreated()
            }
        }

        when (call.method) {
            "create" -> {
                if (players.containsKey(playerId)) {
                    result.errorWithPlayerAlreadyCreated()
                } else {
                    val type = DataSourceType.values()[call.argument("type")!!]
                    val player = ClientAudioPlayer(
                        url = call.argument("url")!!,
                        type = type,
                        playerId = playerId,
                        channel = channel,
                        context = applicationContext,
                        flutterAssets = flutterAssets
                    )
                    players[playerId] = player
                    result.success()
                }
            }
            "play" -> answerWithPlayer { player -> player.play() }
            "pause" -> answerWithPlayer { player -> player.pause() }
            "seek" -> answerWithPlayer { player ->
                val position = call.argument<Long>("position")!!
                player.seek(position)
            }
            "dispose" -> answerWithPlayer { player ->
                player.destroy()
            }
        }

    }

}


@Suppress("NOTHING_TO_INLINE")
private inline fun Result.errorWithPlayerAlreadyCreated() {
    error("1", "player already created.", null)
}

@Suppress("NOTHING_TO_INLINE")
private inline fun Result.errorWithPlayerNotCreated() {
    error("2", "player haven't created.", null)
}

@Suppress("NOTHING_TO_INLINE")
private inline fun Result.success() {
    success(null)
}

private class ClientAudioPlayer(
    url: String,
    private val playerId: String,
    private val channel: MethodChannel,
    type: DataSourceType,
    context: Context,
    flutterAssets: FlutterPlugin.FlutterAssets
) : Player.EventListener {

    companion object {
        private const val TAG = "AudioPlayerPlugin"
    }

    private val player = SimpleExoPlayer.Builder(context).build()

    private var destroyed = false

    private var buffering = false

    private val handler = Handler(Looper.getMainLooper())

    private var lastDispatchedBufferedPosition: Long? = null

    private fun scheduleDispatchBufferedSize() {
        val bufferedPosition = player.bufferedPosition
        if (bufferedPosition != lastDispatchedBufferedPosition) {
            dispatchEvent(
                PlaybackEvent.UpdateBufferPosition,
                params = mapOf("ranges" to listOf(0, bufferedPosition))
            )
            lastDispatchedBufferedPosition = bufferedPosition
        }
        handler.postDelayed(::scheduleDispatchBufferedSize, 200)
    }

    init {
        val uri = when (type) {
            DataSourceType.Url -> Uri.parse(url)
            DataSourceType.File -> Uri.fromFile(File(url))
            DataSourceType.Asset -> Uri.parse("asset:///${flutterAssets.getAssetFilePathByName(url)}")
        }
        val mediaSource = ProgressiveMediaSource
            .Factory(DefaultDataSourceFactory(context))
            .createMediaSource(MediaItem.fromUri(uri))
        player.setMediaSource(mediaSource)
        player.prepare()
        player.addListener(this)
        dispatchEvent(PlaybackEvent.Preparing)
        scheduleDispatchBufferedSize()
    }

    fun destroy() {
        destroyed = true
        player.release()
        handler.removeCallbacksAndMessages(null)
    }

    fun play() {
        if (destroyed) {
            return
        }
        player.playWhenReady = true
    }

    fun pause() {
        if (destroyed) {
            return
        }
        player.playWhenReady = false
    }

    fun seek(position: Long) {
        if (destroyed) {
            return
        }
        dispatchEvent(PlaybackEvent.Seeking)
        player.seekTo(position)
        dispatchEventWithPosition(PlaybackEvent.SeekFinished)
    }

    override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) {
        if (playWhenReady) {
            dispatchEventWithPosition(PlaybackEvent.Playing)
        } else {
            dispatchEventWithPosition(PlaybackEvent.Paused)
        }
    }

    private fun dispatchEventWithPosition(event: PlaybackEvent) {
        dispatchEvent(
            event,
            mapOf(
                "position" to player.currentPosition,
                "updateTime" to SystemClock.uptimeMillis()
            )
        )
    }

    private fun dispatchEvent(event: PlaybackEvent, params: Map<String, Any?> = emptyMap()) {
        val map = params.toMutableMap()
        map["playerId"] = playerId
        map["event"] = event.ordinal
        channel.invokeMethod("onPlaybackEvent", map)
    }

    override fun onPlayerError(error: ExoPlaybackException) {
        Log.d(TAG, "onPlayerError() called with: error = $error")
        dispatchEvent(PlaybackEvent.Error)
    }

    override fun onPlaybackStateChanged(state: Int) {
        when (state) {
            Player.STATE_READY -> {
                if (buffering) {
                    buffering = false
                    dispatchEventWithPosition(PlaybackEvent.BufferingEnd)
                } else {
                    dispatchEvent(PlaybackEvent.Prepared, mapOf("duration" to player.duration))
                }
            }
            Player.STATE_BUFFERING -> {
                dispatchEventWithPosition(PlaybackEvent.BufferingStart)
                buffering = true
            }
            Player.STATE_ENDED, Player.STATE_IDLE -> {
                dispatchEventWithPosition(PlaybackEvent.End)
            }
        }
    }

}


private enum class PlaybackEvent {
    Paused,
    Playing,
    Preparing,
    Prepared,
    BufferingStart,
    BufferingEnd,
    Error,
    Seeking,
    SeekFinished,
    End,
    UpdateBufferPosition,
}

private enum class DataSourceType {
    Url, File, Asset
}
