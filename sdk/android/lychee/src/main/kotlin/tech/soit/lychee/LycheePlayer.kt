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

    /** What a track is for, as the container says. */
    enum class TrackRole(internal val value: Int) {
        MAIN(0),
        /** Spoken commentary (a director's, ...). */
        COMMENTARY(1),
        /** For viewers who cannot see the picture: audio description, or text describing the video. */
        DESCRIPTION(2),
        /** For viewers who cannot hear the sound: captions (SDH), or audio with clearer dialog. */
        HEARING_IMPAIRED(3),
    }

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
        /**
         * An [addSubtitle] request finished: if [added], the file is the
         * subtitle track with that [Track.id], selected ([TracksChanged]
         * follows); otherwise it could not be read or is not a subtitle file.
         */
        data class SubtitleAdded(val id: Int, val added: Boolean) : Event()
        /**
         * Another [Variant] is on screen, its [Variant.index]: after
         * [selectVariant], adaptation, or the first frame.
         */
        data class VariantChanged(val index: Int) : Event()
    }

    fun interface Listener {
        fun onEvent(event: Event)
    }

    /** An audio, video or subtitle stream of the open media. */
    data class Track(
        /** Pass to [selectTrack]. */
        val index: Int,
        val type: TrackType,
        /** The container's stream index; for a subtitle file, the id [addSubtitle] returned. */
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
        /** A subtitle file [addSubtitle] added. */
        val isExternal: Boolean = false,
        /**
         * The container marks it as a track to play by default (Matroska's FlagDefault,
         * which is set unless a file says otherwise; MP4's enabled flag; HLS `DEFAULT=YES`).
         */
        val isDefault: Boolean = false,
        /** Subtitles to show even when subtitles are off, e.g. for dialog in another language. */
        val isForced: Boolean = false,
        val role: TrackRole = TrackRole.MAIN,
        /**
         * Bits per second the container declares or its sample sizes imply (HLS: the
         * variant's `BANDWIDTH`, on its video track); 0 if unknown.
         */
        val bitrate: Long = 0,
    )

    /** A variant of an HLS master playlist that this build can play. */
    data class Variant(
        /** Pass to [selectVariant]. */
        val index: Int,
        /** Declared bit/s. */
        val bandwidth: Long,
        /** Picture size and frame rate; 0 if the playlist does not say. */
        val width: Int,
        val height: Int,
        val frameRate: Double,
        /** The CODECS attribute, or empty. */
        val codecs: String,
        /** The [Track.index] of the video track that plays it. */
        val track: Int,
        val isPlaying: Boolean,
        /** [selectVariant] keeps to it. */
        val isSelected: Boolean,
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
        /**
         * Average of how far ahead of the clock (the audio heard) frames are when shown, in
         * microseconds; negative: behind.
         */
        val videoOffsetUs: Long,
        /** What the audio device plays, 0 without audio, and how far ahead of now it plays. */
        val audioOutputSampleRate: Int,
        val audioOutputChannels: Int,
        val audioOutputLatencyMs: Long,
        /** The bandwidth of the HLS variant on screen, 0 for other input. */
        val variantBandwidthBps: Long,
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
     *
     * Every http(s) request of the input carries [headers] ("User-Agent",
     * "Referer", "Cookie", any other): the URL, its redirects, and for HLS
     * every playlist, segment, key, `EXT-X-MAP` and rendition. Returns
     * false, opening nothing, if a name or value holds a line break.
     */
    @JvmOverloads
    fun open(url: String, headers: Map<String, String> = emptyMap()): Boolean {
        val lines = headerLines(headers) ?: return false
        return nativeOpen(handle(), url.toByteArray(), lines.toByteArray()) == 0
    }

    /**
     * Stops what is open and goes back to [State.IDLE], releasing decoders,
     * the audio output and the input; the player, its listeners, volume,
     * rate, looping and play/pause stay for the next [open]. Unlike
     * [close], the player stays usable.
     */
    fun closeMedia() {
        nativeClose(handle())
    }

    /** Plays from the start again if the state is [State.ENDED]. */
    fun play() = nativePlay(handle())

    fun pause() = nativePause(handle())

    /** Recreates video decoding after platform invalidation, keeping playback state. */
    fun rebuildVideoDecoder() = nativeRebuildVideoDecoder(handle())

    /** Ignored where [isSeekable] is false. */
    fun seekTo(positionMs: Long) = nativeSeek(handle(), positionMs * 1000)

    /**
     * 0 - 1. Kept across [open]. Moves there over 10 ms, so the change does
     * not click, and stops a [fade] under way. After a fade, its target.
     */
    var volume: Float = 1f
        set(value) {
            field = value
            nativeSetVolume(handle(), value)
        }

    /**
     * Moves [volume] to [to] (0 - 1) over [durationMs] of audio played, e.g.
     * to fade a track out before the next one or in after it. Goes on from
     * where a fade under way has got to, and holds while paused; an [open]
     * meanwhile starts at [to].
     */
    fun fade(to: Float, durationMs: Long) {
        volume = to
        nativeFade(handle(), to, durationMs * 1000)
    }

    /**
     * A gain in dB (-60 to +24, 0 by default) on top of [volume], such as a
     * ReplayGain track or album gain. Kept across [open]: set it for each
     * input. A boost is lowered so the loudest sample stays within full
     * scale: [peak], the input's largest sample (1 at full scale), if known
     * (0 if not); otherwise the boost comes down when the decoder first
     * meets a louder sample, ahead of it playing.
     */
    fun setGain(db: Float, peak: Float = 0f) = nativeSetGain(handle(), db, peak)

    /**
     * Fills [samples] with what the audio output played up to
     * [displayTimeUs] (`lychee_now_us()` timebase: [LycheeFrameDriver]'s
     * display times), mixed down to mono, after volume and gain, at
     * [Stats.audioOutputSampleRate]: for visualizers. The first call
     * starts recording what plays, so it and any time the output played
     * nothing read as silence; recording stops a second after the last
     * call. False without an audio track.
     */
    fun getAudioSamples(displayTimeUs: Long, samples: FloatArray): Boolean =
        nativeGetAudioSamples(handle(), displayTimeUs, samples)

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

    /**
     * The positions [seekTo] reaches in milliseconds, null if it cannot
     * seek; the upper bound is null if unknown. Live HLS whose playlist
     * keeps a window counts positions from the start of the playlist's
     * first segment when it was opened; the range moves on in real time,
     * and playback starts at [liveEdgeMs].
     */
    val seekableRangeMs: Pair<Long, Long?>?
        get() {
            val range = LongArray(2)
            if (!nativeGetSeekableRange(handle(), range)) return null
            return range[0] / 1000 to range[1].takeIf { it >= 0 }?.let { it / 1000 }
        }

    /**
     * Live HLS that can seek: the position that plays live, as it moves
     * on; seek there to return to live. Null for other input.
     */
    val liveEdgeMs: Long?
        get() = nativeGetLiveEdge(handle()).takeIf { it >= 0 }?.let { it / 1000 }

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
            val longs = LongArray(12)
            return (0 until nativeGetTrackCount(handle)).mapNotNull { index ->
                val strings = nativeGetTrackInfo(handle, index, longs)?.let(::String)
                    ?.split('\u0000') ?: return@mapNotNull null
                val type = TrackType.entries.firstOrNull { it.value.toLong() == longs[0] }
                    ?: return@mapNotNull null
                Track(
                    index = index, type = type, id = longs[1].toInt(), isSelected = longs[2] != 0L,
                    codec = strings[0], language = strings[1], title = strings[2],
                    width = longs[3].toInt(), height = longs[4].toInt(),
                    sampleRate = longs[5].toInt(), channels = longs[6].toInt(),
                    isExternal = longs[7] != 0L, isDefault = longs[8] != 0L,
                    isForced = longs[9] != 0L,
                    role = TrackRole.entries.firstOrNull { it.value.toLong() == longs[10] }
                        ?: TrackRole.MAIN,
                    bitrate = longs[11],
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

    /** The variants of an HLS master playlist, from [Event.MediaInfo] on; empty for other media. */
    val variants: List<Variant>
        get() {
            val handle = handle()
            val longs = LongArray(7)
            return (0 until nativeGetVariantCount(handle)).mapNotNull { index ->
                val codecs = nativeGetVariantInfo(handle, index, longs)?.let(::String)
                    ?: return@mapNotNull null
                Variant(
                    index = index, bandwidth = longs[0], width = longs[1].toInt(),
                    height = longs[2].toInt(), frameRate = Double.fromBits(longs[3]),
                    codecs = codecs, track = longs[4].toInt(), isPlaying = longs[5] != 0L,
                    isSelected = longs[6] != 0L,
                )
            }
        }

    /**
     * Plays [Variant.index] only, or adapts to the bandwidth again for
     * null. Returns false if the request was rejected;
     * [Event.VariantChanged] follows once it is on screen.
     */
    fun selectVariant(index: Int?): Boolean = nativeSelectVariant(handle(), index ?: -1)

    /**
     * The highest bandwidth (bit/s) automatic variant selection plays, or
     * null for no limit. Kept across [open].
     */
    var maxBitrate: Long? = null
        set(value) {
            field = value
            nativeSetMaxBitrate(handle(), (value ?: 0).coerceAtLeast(0))
        }

    /**
     * Reads the subtitle file [url] (a path, file:// or http(s):// URL;
     * SubRip, WebVTT or ASS/SSA) in the background and adds it to the media
     * opened last as a subtitle track after the container's, its cue times
     * counting from the start of the media. Once loaded it is selected,
     * unless [select] is false: then the subtitle track selected before (or
     * none) stays. [Event.SubtitleAdded] with the returned id says how it
     * went. Opening other media drops it. Returns null if nothing is open,
     * or if [headers] (HTTP request headers for [url], as in [open]; the
     * media's are not used) holds a line break.
     *
     * Without a byte order mark, the text is in [encoding], a name
     * [java.nio.charset.Charset] knows such as "GB18030", "Big5",
     * "Shift_JIS" or "windows-1251"; null reads UTF-8, or guesses between
     * GB18030 (GBK), Big5 and windows-1252 when the file is not UTF-8.
     */
    @JvmOverloads
    fun addSubtitle(
        url: String,
        language: String? = null,
        title: String? = null,
        encoding: String? = null,
        select: Boolean = true,
        headers: Map<String, String> = emptyMap(),
    ): Int? {
        val lines = headerLines(headers) ?: return null
        return nativeAddSubtitle(
            handle(), url.toByteArray(), language?.toByteArray(), title?.toByteArray(),
            encoding?.toByteArray(), lines.toByteArray(), !select,
        ).takeIf { it >= 0 }
    }

    /**
     * Shows the subtitles of every track this many milliseconds later;
     * negative shows them earlier (a container's only as far as the read
     * is ahead, usually a few seconds). Kept across [open].
     */
    var subtitleDelayMs: Long = 0
        set(value) {
            field = value
            nativeSetSubtitleDelay(handle(), value * 1000)
        }

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
            val longs = LongArray(20)
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
                videoOffsetUs = longs[15], audioOutputSampleRate = longs[16].toInt(),
                audioOutputChannels = longs[17].toInt(), audioOutputLatencyMs = longs[18] / 1000,
                variantBandwidthBps = longs[19],
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
            9 -> Event.SubtitleAdded(arg0.toInt(), arg1 != 0L)
            10 -> Event.VariantChanged(arg0.toInt())
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
        @JvmStatic private external fun nativeOpen(handle: Long, url: ByteArray, headers: ByteArray): Int
        @JvmStatic private external fun nativeClose(handle: Long): Int
        @JvmStatic private external fun nativePlay(handle: Long)
        @JvmStatic private external fun nativePause(handle: Long)
        @JvmStatic private external fun nativeRebuildVideoDecoder(handle: Long)
        @JvmStatic private external fun nativeSeek(handle: Long, positionUs: Long)
        @JvmStatic private external fun nativeSetVolume(handle: Long, volume: Float)
        @JvmStatic private external fun nativeFade(handle: Long, volume: Float, durationUs: Long)
        @JvmStatic private external fun nativeSetGain(handle: Long, db: Float, peak: Float)
        @JvmStatic private external fun nativeGetAudioSamples(
            handle: Long, displayTimeUs: Long, samples: FloatArray,
        ): Boolean
        @JvmStatic private external fun nativeSetRate(handle: Long, rate: Double)
        @JvmStatic private external fun nativeGetRate(handle: Long): Double
        @JvmStatic private external fun nativeSetLooping(handle: Long, looping: Boolean)
        @JvmStatic private external fun nativeIsLooping(handle: Long): Boolean
        @JvmStatic private external fun nativeIsSeekable(handle: Long): Boolean
        @JvmStatic private external fun nativeGetSeekableRange(handle: Long, range: LongArray): Boolean
        @JvmStatic private external fun nativeGetLiveEdge(handle: Long): Long
        @JvmStatic private external fun nativeGetState(handle: Long): Int
        @JvmStatic private external fun nativeIsPlaying(handle: Long): Boolean
        @JvmStatic private external fun nativeGetPosition(handle: Long): Long
        @JvmStatic private external fun nativeGetDuration(handle: Long): Long
        @JvmStatic private external fun nativeGetBufferedPosition(handle: Long): Long
        @JvmStatic private external fun nativeGetVideoSize(handle: Long): Long
        @JvmStatic private external fun nativeGetRotation(handle: Long): Int
        @JvmStatic private external fun nativeGetError(handle: Long): ByteArray?
        @JvmStatic private external fun nativeGetTrackCount(handle: Long): Int
        @JvmStatic private external fun nativeGetTrackInfo(handle: Long, index: Int, longs: LongArray): ByteArray?
        @JvmStatic private external fun nativeSelectTrack(handle: Long, type: Int, index: Int): Boolean
        @JvmStatic private external fun nativeGetVariantCount(handle: Long): Int
        @JvmStatic private external fun nativeGetVariantInfo(handle: Long, index: Int, longs: LongArray): ByteArray?
        @JvmStatic private external fun nativeSetMaxBitrate(handle: Long, bitrate: Long)
        @JvmStatic private external fun nativeSelectVariant(handle: Long, index: Int): Boolean
        @JvmStatic private external fun nativeAddSubtitle(
            handle: Long, url: ByteArray, language: ByteArray?, title: ByteArray?,
            encoding: ByteArray?, headers: ByteArray, keepSelection: Boolean,
        ): Int

        /** [headers] as the "Name: value" lines the C API takes, or null if one would start another header. */
        private fun headerLines(headers: Map<String, String>): String? {
            val lines = StringBuilder()
            for ((name, value) in headers) {
                if (name.isEmpty() || name.any { it == '\r' || it == '\n' || it == ':' } ||
                    value.any { it == '\r' || it == '\n' }
                ) {
                    return null
                }
                lines.append(name).append(": ").append(value).append("\r\n")
            }
            return lines.toString()
        }
        @JvmStatic private external fun nativeSetSubtitleDelay(handle: Long, delayUs: Long)
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
