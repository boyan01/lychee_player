package tech.soit.lychee

import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.util.Size
import java.lang.ref.WeakReference
import java.nio.ByteBuffer

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
        /**
         * [subtitle] has the new text. With [pictures], the subtitle is drawn
         * from [subtitlePictures] instead, text included; [LycheePlayerView]
         * does that.
         */
        data class SubtitleChanged(val pictures: Boolean) : Event()
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

    /** Counts are since the open. */
    data class Stats(
        val hardwareDecoding: Boolean,
        /** Frames shown, and frames skipped because a later one was due. */
        val framesRendered: Long,
        val framesDropped: Long,
        val videoDecoder: String,
        /** Media read ahead of the playback position. */
        val videoBufferedMs: Long,
        val audioBufferedMs: Long,
        /** Average time decoding one video packet takes, in microseconds. */
        val videoDecodeTimeUs: Long,
        /** "none" without audio. */
        val audioDecoder: String,
        /** Frames the video decoder output, including those skipped to reach a seek target. */
        val framesDecoded: Long,
        /** Bytes read for the selected video and audio tracks. */
        val videoBytesRead: Long,
        val audioBytesRead: Long,
        /** Bits per second the input last arrived at over the network, -1 if unknown. */
        val downloadRateBps: Long,
        /** From the open to the first [State.READY]; 0 before. */
        val startupTimeMs: Long,
        /** Times playback ran out of data while playing, and the time spent so. */
        val rebufferCount: Long,
        val rebufferTimeMs: Long,
        /** Seeks completed, and how long the last took. */
        val seekCount: Long,
        val lastSeekTimeMs: Long,
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

    /** Recreates video decoding after platform invalidation, keeping playback state. */
    fun rebuildVideoDecoder() = nativeRebuildVideoDecoder(handle())

    /** Ignored where [isSeekable] is false. */
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

    /**
     * Whether a seek can move playback: false until [Event.MediaInfo], for
     * live input and at [State.ERROR]. An http server without range
     * requests cannot seek.
     */
    val isSeekable: Boolean
        get() = nativeIsSeekable(handle())

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

    /**
     * Bitmap subtitles (PGS, DVB, VobSub), laid out on a canvas the video
     * fills.
     */
    class SubtitlePictures(
        /** The same for the same pictures, different otherwise. */
        val id: Long,
        /** Premultiplied bitmaps and where they go on the canvas. */
        val pictures: List<Pair<Bitmap, Rect>>,
    )

    /**
     * The subtitle pictures on screen now on a [width] x [height] canvas
     * (0 x 0: the video's size); null if there are none.
     */
    fun subtitlePictures(width: Int = 0, height: Int = 0): SubtitlePictures? {
        val subtitle = nativeAcquireSubtitle(handle(), width, height)
        if (subtitle == 0L) return null
        try {
            val ints = IntArray(6)
            val pictures = (0 until nativeSubtitleImageCount(subtitle)).mapNotNull { index ->
                val pixels = nativeSubtitleImage(subtitle, index, ints) ?: return@mapNotNull null
                // ARGB_8888 is premultiplied R, G, B, A bytes, as the core's.
                val bitmap = Bitmap.createBitmap(ints[4], ints[5], Bitmap.Config.ARGB_8888)
                bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))
                bitmap to Rect(ints[0], ints[1], ints[0] + ints[2], ints[1] + ints[3])
            }
            return SubtitlePictures(nativeSubtitleId(subtitle), pictures)
        } finally {
            nativeSubtitleRelease(subtitle)
        }
    }

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
            val longs = LongArray(15)
            val decoders = String(nativeGetStats(handle(), longs)).split('\n')
            return Stats(
                hardwareDecoding = longs[0] != 0L, framesRendered = longs[1],
                framesDropped = longs[2], videoDecoder = decoders[0],
                videoBufferedMs = longs[3] / 1000, audioBufferedMs = longs[4] / 1000,
                videoDecodeTimeUs = longs[5], audioDecoder = decoders.getOrElse(1) { "none" },
                framesDecoded = longs[6], videoBytesRead = longs[7], audioBytesRead = longs[8],
                downloadRateBps = longs[9], startupTimeMs = longs[10] / 1000,
                rebufferCount = longs[11], rebufferTimeMs = longs[12] / 1000,
                seekCount = longs[13], lastSeekTimeMs = longs[14] / 1000,
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
            8 -> Event.SubtitleChanged(arg1 != 0L)
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

    companion object {
        /**
         * Destroys a player created through the C ABI rather than this
         * class, whose owner is gone: the Flutter plugin's, after a hot
         * restart or with its engine. Its event callback is dropped first.
         * Stop pulling its frames before.
         */
        @JvmStatic
        fun destroyHandle(handle: Long) = nativeDestroy(handle, 0)

        private val mainHandler = Handler(Looper.getMainLooper())

        init {
            // By name first, so the core's JNI_OnLoad keeps the VM for its
            // http client; lychee_jni's dependency alone would not run it.
            System.loadLibrary("lychee")
            System.loadLibrary("lychee_jni")
        }

        @JvmStatic private external fun nativeCreate(hardwareDecoding: Boolean, video: Boolean): Long
        @JvmStatic private external fun nativeSetEventSink(handle: Long, sink: Any): Long
        @JvmStatic private external fun nativeDestroy(handle: Long, sink: Long)
        @JvmStatic private external fun nativeOpen(handle: Long, url: ByteArray): Int
        @JvmStatic private external fun nativePlay(handle: Long)
        @JvmStatic private external fun nativePause(handle: Long)
        @JvmStatic private external fun nativeRebuildVideoDecoder(handle: Long)
        @JvmStatic private external fun nativeSeek(handle: Long, positionUs: Long)
        @JvmStatic private external fun nativeSetVolume(handle: Long, volume: Float)
        @JvmStatic private external fun nativeSetRate(handle: Long, rate: Double)
        @JvmStatic private external fun nativeGetRate(handle: Long): Double
        @JvmStatic private external fun nativeSetLooping(handle: Long, looping: Boolean)
        @JvmStatic private external fun nativeIsLooping(handle: Long): Boolean
        @JvmStatic private external fun nativeIsSeekable(handle: Long): Boolean
        @JvmStatic private external fun nativeGetState(handle: Long): Int
        @JvmStatic private external fun nativeIsPlaying(handle: Long): Boolean
        @JvmStatic private external fun nativeGetPosition(handle: Long): Long
        @JvmStatic private external fun nativeGetDuration(handle: Long): Long
        @JvmStatic private external fun nativeGetBufferedPosition(handle: Long): Long
        @JvmStatic private external fun nativeGetVideoSize(handle: Long): Long
        @JvmStatic private external fun nativeGetRotation(handle: Long): Int
        @JvmStatic private external fun nativeGetError(handle: Long): ByteArray?
        @JvmStatic private external fun nativeGetTrackCount(handle: Long): Int
        @JvmStatic private external fun nativeGetTrackInfo(handle: Long, index: Int, ints: IntArray): ByteArray?
        @JvmStatic private external fun nativeSelectTrack(handle: Long, type: Int, index: Int): Boolean
        @JvmStatic private external fun nativeGetSubtitle(handle: Long): ByteArray?
        @JvmStatic private external fun nativeGetMetadata(handle: Long, key: ByteArray): ByteArray?
        @JvmStatic private external fun nativeGetArtwork(handle: Long): ByteArray?
        @JvmStatic private external fun nativeAcquireSubtitle(handle: Long, width: Int, height: Int): Long
        @JvmStatic private external fun nativeSubtitleRelease(subtitle: Long)
        @JvmStatic private external fun nativeSubtitleId(subtitle: Long): Long
        @JvmStatic private external fun nativeSubtitleImageCount(subtitle: Long): Int
        @JvmStatic private external fun nativeSubtitleImage(subtitle: Long, index: Int, ints: IntArray): ByteArray?
        @JvmStatic private external fun nativeGetStats(handle: Long, longs: LongArray): ByteArray
    }
}
