package tech.soit.flutter.audio_player

import android.media.MediaPlayer
import android.os.SystemClock
import android.util.Log
import androidx.annotation.NonNull
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

/** AudioPlayerPlugin */
class AudioPlayerPlugin : FlutterPlugin, MethodCallHandler {

    companion object {
        private const val TAG = "AudioPlayerPlugin"
    }

    private lateinit var channel: MethodChannel

    private val players = mutableMapOf<String, ClientAudioPlayer>()

    override fun onAttachedToEngine(
        @NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding
    ) {
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
                    val player = ClientAudioPlayer(
                        url = call.argument("url")!!,
                        playerId = playerId,
                        channel = channel
                    )
                    players[playerId] = player
                    result.success()
                }
            }
            "play" -> {
                answerWithPlayer { player -> player.play() }
            }
            "pause" -> {
                answerWithPlayer { player -> player.pause() }
            }
            "seek" -> {
                answerWithPlayer { player ->
                    val position = call.argument<Long>("position")!!
                    player.seek(position)
                }
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
    private val channel: MethodChannel
) : MediaPlayer.OnPreparedListener, MediaPlayer.OnInfoListener, MediaPlayer.OnErrorListener,
    MediaPlayer.OnCompletionListener {

    companion object {
        private const val TAG = "AudioPlayerPlugin"
    }

    private val player = MediaPlayer()

    private var prepared = false

    private var destoried = false

    init {
        player.setDataSource(url)
        player.setOnPreparedListener(this)
        player.setOnInfoListener(this)
        player.setOnErrorListener(this)
        player.setOnCompletionListener(this)
        dispatchEvent(PlaybackEvent.Preparing)
        player.prepareAsync()
    }

    fun destroy() {
        destoried = true
        player.release()
    }

    fun play() {
        if (destoried) {
            return
        }
        if (prepared) {
            player.start()
            dispatchEventWithPosition(PlaybackEvent.Playing)
        }
    }

    fun pause() {
        if (destoried) {
            return
        }
        if (player.isPlaying) {
            player.pause()
            dispatchEventWithPosition(PlaybackEvent.Paused)
        }
    }

    fun seek(position: Long) {
        if (destoried) {
            return
        }
        if (prepared) {
            dispatchEvent(PlaybackEvent.Seeking)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                player.seekTo(position, MediaPlayer.SEEK_CLOSEST)
            } else {
                player.seekTo(position.toInt())
            }
            dispatchEventWithPosition(PlaybackEvent.SeekFinished)
        }
    }


    override fun onPrepared(mp: MediaPlayer) {
        dispatchEvent(PlaybackEvent.Prepared, mapOf("duration" to mp.duration))
        prepared = true
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

    override fun onInfo(mp: MediaPlayer, what: Int, extra: Int): Boolean {
        if (what == MediaPlayer.MEDIA_INFO_BUFFERING_START) {
            dispatchEventWithPosition(PlaybackEvent.Buffering)
        } else if (what == MediaPlayer.MEDIA_INFO_BUFFERING_END) {
            dispatchEventWithPosition(PlaybackEvent.Prepared)
        }
        return false
    }

    override fun onError(mp: MediaPlayer, what: Int, extra: Int): Boolean {
        dispatchEvent(PlaybackEvent.Error)
        return false
    }

    override fun onCompletion(mp: MediaPlayer) {
        dispatchEventWithPosition(PlaybackEvent.End)
    }

}


private enum class PlaybackEvent {
    Paused,
    Playing,
    Preparing,
    Prepared,
    Buffering,
    Error,
    Seeking,
    SeekFinished,
    End,
}
