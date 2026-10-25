package tech.soit.lychee

import android.os.Handler
import android.os.Looper
import android.util.Size
import java.lang.ref.WeakReference

/**
 * A media player: lychee_player behind a Kotlin interface. Show its video
 * with [LycheePlayerView], or pull frames yourself with [LycheeFrameDriver].
 *
 * Methods can be called from any thread, until [close]. Listeners run on
 * the main thread. Times are in milliseconds.
 *
 * Video comes upright: MediaCodec rotates what it renders
 * (lychee_config.rotate_surface_frames), so callers do not rotate it again.
 */
class LycheePlayer @JvmOverloads constructor(
    hardwareDecoding: Boolean = true,
    /** False plays audio only; video tracks are listed but never decoded. */
    video: Boolean = true,
) : AutoCloseable {
    /** Matches lychee_state. */
    enum class State { IDLE, LOADING, READY, BUFFERING, ENDED, ERROR }

    /** Matches lychee_track_type. */
    enum class TrackType(internal val value: Int) { VIDEO(1), AUDIO(2), SUBTITLE(3) }

    sealed class Event {
        data class StateChanged(val state: State) : Event()
        data class PlayingChanged(val playing: Boolean) : Event()
        /** Duration, tracks and video size are available. */
        data object MediaInfo : Event()
        data class VideoSizeChanged(val size: Size) : Event()
        /** The position the seek landed on. */
        data class SeekCompleted(val positionMs: Long) : Event()
        /** [error] says which. */
        data object Error : Event()
        data object TracksChanged : Event()
        /** [subtitle] has the new text. */
        data object SubtitleChanged : Event()
    }

    fun interface Listener {
        fun onEvent(event: Event)
    }

    /** An audio, video or subtitle stream of the open media. */
    data class Track(
        /** Pass to [selectTrack]. */
        val index: Int,
        val type: TrackType,
        /** The container's stream index. */
        val id: Int,
        val isSelected: Boolean,
        val codec: String,
        val language: String,
        val title: String,
        /** Video only: display size. */
        val width: Int,
        val height: Int,
        /** Audio only. */
        val sampleRate: Int,
        val channels: Int,
    )

    data class Stats(
        val hardwareDecoding: Boolean,
        val framesRendered: Long,
        val framesDropped: Long,
        val videoDecoder: String,
        /** Media read ahead of the playback position. */
        val videoBufferedMs: Long,
        val audioBufferedMs: Long,
        /** Average time decoding one video packet takes, in microseconds. */
        val videoDecodeTimeUs: Long,
    )

    /** The lychee_player*, for the C ABI and [LycheeFrameDriver]. */
    val nativeHandle: Long = nativeCreate(hardwareDecoding, video)

    private val listeners = ArrayList<Listener>()  // Main thread.
    private val sink = EventSink(this)
    private val sinkRef = nativeSetEventSink(nativeHandle, sink)

    @Volatile
    private var closed = false

    /** Main thread. */
    fun addListener(listener: Listener) {
        listeners += listener
    }

    /** Main thread. */
    fun removeListener(listener: Listener) {
        listeners -= listener
    }

    /**
     * Opens a file path or URL. Calling it again replaces the current media;
     * volume, rate, looping and play/pause are kept.
     */
    fun open(url: String) {
        nativeOpen(handle(), url.toByteArray())
    }

    /** Plays from the start again if the state is [State.ENDED]. */
    fun play() = nativePlay(handle())

    fun pause() = nativePause(handle())

    fun seekTo(positionMs: Long) = nativeSeek(handle(), positionMs * 1000)

    /** 0 - 1. */
    var volume: Float = 1f
        set(value) {
            field = value
            nativeSetVolume(handle(), value)
        }

    /** Playback speed, clamped to 0.25 - 4. Audio keeps its pitch. */
    var rate: Double
        get() = nativeGetRate(handle())
        set(value) = nativeSetRate(handle(), value)

    /** At the end, play again from the start instead of ending. */
    var isLooping: Boolean
        get() = nativeIsLooping(handle())
        set(value) = nativeSetLooping(handle(), value)

    val state: State
        get() = State.entries.getOrElse(nativeGetState(handle())) { State.IDLE }

    /** Whether playback is requested; stays true at [State.ENDED]. */
    val isPlaying: Boolean
        get() = nativeIsPlaying(handle())

    val positionMs: Long
        get() = nativeGetPosition(handle()) / 1000

    /** Null if unknown. */
    val durationMs: Long?
        get() = nativeGetDuration(handle()).takeIf { it >= 0 }?.let { it / 1000 }

    val bufferedPositionMs: Long
        get() = nativeGetBufferedPosition(handle()) / 1000

    /** Display size, before rotation; null if there is no video. */
    val videoSize: Size?
        get() = nativeGetVideoSize(handle()).takeIf { it != 0L }
            ?.let { Size((it ushr 32).toInt(), it.toInt()) }

    /** Clockwise rotation of the media in degrees: 0, 90, 180 or 270. */
    val rotation: Int
        get() = nativeGetRotation(handle())

    /** The last error, once the state is [State.ERROR]. */
    val error: String?
        get() = if (state == State.ERROR) nativeGetError(handle())?.let(::String) else null

    val tracks: List<Track>
        get() {
            val handle = handle()
            val ints = IntArray(7)
            return (0 until nativeGetTrackCount(handle)).mapNotNull { index ->
                val strings = nativeGetTrackInfo(handle, index, ints)?.let(::String)
                    ?.split('\u0000') ?: return@mapNotNull null
                val type = TrackType.entries.firstOrNull { it.value == ints[0] }
                    ?: return@mapNotNull null
                Track(
                    index = index, type = type, id = ints[1], isSelected = ints[2] != 0,
                    codec = strings[0], language = strings[1], title = strings[2],
                    width = ints[3], height = ints[4], sampleRate = ints[5], channels = ints[6],
                )
            }
        }

    /**
     * Switches the [type] track to [Track.index], or disables [type] if
     * [index] is null. Returns false if the request was rejected;
     * [Event.TracksChanged] follows otherwise.
     */
    fun selectTrack(type: TrackType, index: Int?): Boolean =
        nativeSelectTrack(handle(), type.value, index ?: -1)

    /** The subtitle text on screen now, lines separated by "\n". */
    val subtitle: String?
        get() = nativeGetSubtitle(handle())?.let(::String)

    /** The container tag [key] ("title", "artist", ...; case-insensitive). */
    fun metadata(key: String): String? =
        nativeGetMetadata(handle(), key.toByteArray())?.let(::String)

    /**
     * The cover picture as the media stores it, usually JPEG or PNG (for
     * `BitmapFactory.decodeByteArray`); null if it has none.
     */
    val artwork: ByteArray?
        get() = nativeGetArtwork(handle())

    val stats: Stats
        get() {
            val longs = LongArray(6)
            val decoder = String(nativeGetStats(handle(), longs))
            return Stats(
                hardwareDecoding = longs[0] != 0L, framesRendered = longs[1],
                framesDropped = longs[2], videoDecoder = decoder,
                videoBufferedMs = longs[3] / 1000, audioBufferedMs = longs[4] / 1000,
                videoDecodeTimeUs = longs[5],
            )
        }

    /**
     * Destroys the player. Stop pulling its frames first: detach it from
     * every [LycheePlayerView] and close its [LycheeFrameDriver]s.
     */
    override fun close() {
        if (closed) return
        closed = true
        nativeDestroy(nativeHandle, sinkRef)
    }

    private fun handle(): Long {
        check(!closed) { "LycheePlayer is closed" }
        return nativeHandle
    }

    // Main thread.
    private fun dispatch(type: Int, arg0: Long, arg1: Long) {
        if (closed) return
        val event = when (type) {
            1 -> State.entries.getOrNull(arg0.toInt())?.let { Event.StateChanged(it) }
            2 -> Event.PlayingChanged(arg0 != 0L)
            3 -> Event.MediaInfo
            4 -> Event.VideoSizeChanged(Size(arg0.toInt(), arg1.toInt()))
            5 -> Event.SeekCompleted(arg0 / 1000)
            6 -> Event.Error
            7 -> Event.TracksChanged
            8 -> Event.SubtitleChanged
            else -> null
        } ?: return
        // A copy: listeners may remove themselves.
        listeners.toList().forEach { it.onEvent(event) }
    }

    // What the native callback holds: the player only weakly, so a player
    // nobody holds is not kept alive by it.
    private class EventSink(player: LycheePlayer) {
        private val player = WeakReference(player)

        // The player's threads.
        @Suppress("unused")
        fun onEvent(type: Int, arg0: Long, arg1: Long) {
            mainHandler.post { player.get()?.dispatch(type, arg0, arg1) }
        }
    }

    private companion object {
        val mainHandler = Handler(Looper.getMainLooper())

        init {
            System.loadLibrary("lychee_jni")
        }

        @JvmStatic external fun nativeCreate(hardwareDecoding: Boolean, video: Boolean): Long
        @JvmStatic external fun nativeSetEventSink(handle: Long, sink: Any): Long
        @JvmStatic external fun nativeDestroy(handle: Long, sink: Long)
        @JvmStatic external fun nativeOpen(handle: Long, url: ByteArray): Int
        @JvmStatic external fun nativePlay(handle: Long)
        @JvmStatic external fun nativePause(handle: Long)
        @JvmStatic external fun nativeSeek(handle: Long, positionUs: Long)
        @JvmStatic external fun nativeSetVolume(handle: Long, volume: Float)
        @JvmStatic external fun nativeSetRate(handle: Long, rate: Double)
        @JvmStatic external fun nativeGetRate(handle: Long): Double
        @JvmStatic external fun nativeSetLooping(handle: Long, looping: Boolean)
        @JvmStatic external fun nativeIsLooping(handle: Long): Boolean
        @JvmStatic external fun nativeGetState(handle: Long): Int
        @JvmStatic external fun nativeIsPlaying(handle: Long): Boolean
        @JvmStatic external fun nativeGetPosition(handle: Long): Long
        @JvmStatic external fun nativeGetDuration(handle: Long): Long
        @JvmStatic external fun nativeGetBufferedPosition(handle: Long): Long
        @JvmStatic external fun nativeGetVideoSize(handle: Long): Long
        @JvmStatic external fun nativeGetRotation(handle: Long): Int
        @JvmStatic external fun nativeGetError(handle: Long): ByteArray?
        @JvmStatic external fun nativeGetTrackCount(handle: Long): Int
        @JvmStatic external fun nativeGetTrackInfo(handle: Long, index: Int, ints: IntArray): ByteArray?
        @JvmStatic external fun nativeSelectTrack(handle: Long, type: Int, index: Int): Boolean
        @JvmStatic external fun nativeGetSubtitle(handle: Long): ByteArray?
        @JvmStatic external fun nativeGetMetadata(handle: Long, key: ByteArray): ByteArray?
        @JvmStatic external fun nativeGetArtwork(handle: Long): ByteArray?
        @JvmStatic external fun nativeGetStats(handle: Long, longs: LongArray): ByteArray
    }
}
