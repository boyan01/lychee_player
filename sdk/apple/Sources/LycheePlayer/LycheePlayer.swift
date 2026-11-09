import CoreGraphics
import Foundation
import lychee

/// A media player: lychee_player behind a Swift interface. Show its video
/// with ``LycheePlayerView``, or pull frames yourself with
/// ``LycheeFrameDriver``.
///
/// Methods can be called from any thread. Events arrive on the main queue.
public final class LycheePlayer {
  /// Matches lychee_state.
  public enum State: Int32 {
    case idle, loading, ready, buffering, ended, error
  }

  /// Matches lychee_track_type.
  public enum TrackType: Int32 {
    case video = 1, audio, subtitle
  }

  public enum Event {
    case stateChanged(State)
    case playingChanged(Bool)
    /// Duration, tracks and video size are available.
    case mediaInfo
    case videoSizeChanged(CGSize)
    /// The position the seek landed on.
    case seekCompleted(TimeInterval)
    /// ``error`` says which.
    case error
    case tracksChanged
    /// ``subtitle`` has the new text. With `pictures`, the subtitle is drawn
    /// from ``subtitlePictures(width:height:)`` instead, text included;
    /// ``LycheePlayerView`` does that.
    case subtitleChanged(pictures: Bool)
    /// An ``addSubtitle(_:language:title:)`` request finished: if `added`,
    /// the file is the subtitle track with that ``Track/id``, selected
    /// (``tracksChanged`` follows); otherwise it could not be read or is
    /// not a subtitle file.
    case subtitleAdded(id: Int, added: Bool)
    /// Another ``Variant`` is on screen, its ``Variant/index``: after
    /// ``selectVariant(_:)``, adaptation, or the first frame.
    case variantChanged(Int)
    /// The media ``setNext(_:)`` set is now the media open: if `gapless`, it
    /// went on from the media before without a gap; otherwise the player
    /// switched to it (or it failed to open, and ``error`` follows).
    /// ``mediaInfo`` follows.
    case itemChanged(gapless: Bool)
  }

  /// What a track is for, as the container says. Matches
  /// lychee_track_role.
  public enum TrackRole: Int32 {
    case main = 0
    /// Spoken commentary (a director's, ...).
    case commentary
    /// For viewers who cannot see the picture: audio description, or text
    /// describing the video.
    case description
    /// For viewers who cannot hear the sound: captions (SDH), or audio
    /// with clearer dialog.
    case hearingImpaired
  }

  /// An audio, video or subtitle stream of the open media.
  public struct Track {
    /// Pass to ``selectTrack(_:index:)``.
    public let index: Int
    public let type: TrackType
    /// The container's stream index; for a subtitle file, the id
    /// ``addSubtitle(_:language:title:)`` returned.
    public let id: Int
    public let isSelected: Bool
    public let codec: String
    public let language: String
    public let title: String
    /// Video only: display size.
    public let width: Int
    public let height: Int
    /// Audio only.
    public let sampleRate: Int
    public let channels: Int
    /// A subtitle file ``addSubtitle(_:language:title:)`` added.
    public let isExternal: Bool
    /// The container marks it as a track to play by default (Matroska's
    /// FlagDefault, which is set unless a file says otherwise; MP4's
    /// enabled flag; HLS `DEFAULT=YES`).
    public let isDefault: Bool
    /// Subtitles to show even when subtitles are off, e.g. for dialog in
    /// another language.
    public let isForced: Bool
    public let role: TrackRole
    /// Bits per second the container declares or its sample sizes imply
    /// (HLS: the variant's `BANDWIDTH`, on its video track); 0 if unknown.
    public let bitrate: Int64
  }

  /// A variant of an HLS master playlist that this build can play.
  public struct Variant {
    /// Pass to ``selectVariant(_:)``.
    public let index: Int
    /// Declared bit/s.
    public let bandwidth: Int64
    /// Picture size and frame rate; 0 if the playlist does not say.
    public let width: Int
    public let height: Int
    public let frameRate: Double
    /// The CODECS attribute, or empty.
    public let codecs: String
    /// The ``Track/index`` of the video track that plays it.
    public let track: Int
    public let isPlaying: Bool
    /// ``selectVariant(_:)`` keeps to it.
    public let isSelected: Bool
  }

  /// Bitmap subtitles (PGS, DVB, VobSub), laid out on a canvas the video
  /// fills.
  public struct SubtitlePictures {
    /// The same for the same pictures, different otherwise.
    public let id: Int64
    /// Premultiplied pictures and where they go on the canvas, y down.
    public let pictures: [(image: CGImage, rect: CGRect)]
  }

  /// Counts are since the open.
  public struct Stats {
    public let hardwareDecoding: Bool
    /// Frames shown, and frames skipped because a later one was due.
    public let framesRendered: Int
    public let framesDropped: Int
    public let videoDecoder: String
    /// Media read ahead of the playback position.
    public let videoBuffered: TimeInterval
    public let audioBuffered: TimeInterval
    /// Average time decoding one video packet takes.
    public let videoDecodeTime: TimeInterval
    /// "none" without audio.
    public let audioDecoder: String
    /// Frames the video decoder output, including those skipped to reach a
    /// seek target.
    public let framesDecoded: Int
    /// Bytes read for the selected video and audio tracks.
    public let videoBytesRead: Int64
    public let audioBytesRead: Int64
    /// Bits per second the input last arrived at over the network, nil if
    /// unknown.
    public let downloadRate: Int64?
    /// From the open to the first `.ready`; 0 before.
    public let startupTime: TimeInterval
    /// Times playback ran out of data while playing, and the time spent so.
    public let rebufferCount: Int
    public let rebufferTime: TimeInterval
    /// Seeks completed, and how long the last took.
    public let seekCount: Int
    public let lastSeekTime: TimeInterval
    /// Average of how far ahead of the clock (the audio heard) frames are
    /// when shown; negative: behind.
    public let videoOffset: TimeInterval
    /// What the audio device plays, 0 without audio, and how far ahead of
    /// now it plays what it takes.
    public let audioOutputSampleRate: Int
    public let audioOutputChannels: Int
    public let audioOutputLatency: TimeInterval
    /// The bandwidth (bit/s) of the HLS variant on screen, nil for other
    /// input.
    public let variantBandwidth: Int64?
  }

  /// Called on the main queue.
  public var onEvent: ((Event) -> Void)?
  // The LycheePlayerView showing this player, besides onEvent, which stays
  // the app's: subtitle pictures change without a new frame when paused,
  // and new media without video brings no frame to replace the last one.
  var onEventForView: ((Event) -> Void)?

  let handle: OpaquePointer
  // Holds onEvent's owner weakly, so callbacks still queued when the player
  // goes away find nothing.
  private let sink = EventSink()

  /// - Parameters:
  ///   - hardwareDecoding: try VideoToolbox first.
  ///   - video: false plays audio only; video tracks are listed but never
  ///     decoded.
  public init(hardwareDecoding: Bool = true, video: Bool = true) {
    var config = lychee_config()
    lychee_config_init(&config, UInt32(MemoryLayout<lychee_config>.size))
    // The decoder's own buffers: 10-bit and HDR as they are.
    config.video_frame_types = video ? LYCHEE_FRAME_CVPIXELBUFFER_NATIVE.rawValue : 0
    config.hardware_decoding = hardwareDecoding ? 1 : 0
    handle = lychee_player_create(&config)
    sink.player = self
    lychee_player_set_event_callback(handle, { userData, type, arg0, arg1 in
      let sink = Unmanaged<EventSink>.fromOpaque(userData!).takeUnretainedValue()
      DispatchQueue.main.async { sink.deliver(type, arg0, arg1) }
    }, Unmanaged.passUnretained(sink).toOpaque())
  }

  deinit {
    // Joins the player's threads: no callback runs after this.
    lychee_player_destroy(handle)
  }

  /// Opens a file path or URL. Calling it again replaces the current media;
  /// volume, rate, looping and play/pause are kept.
  ///
  /// Every http(s) request of the input carries `headers` ("User-Agent",
  /// "Referer", "Cookie", any other): the URL, its redirects, and for HLS
  /// every playlist, segment, key, `EXT-X-MAP` and rendition. Returns false,
  /// opening nothing, if a name or value holds a line break.
  ///
  /// With a `start` above 0, playback starts there: nothing before it is
  /// shown or heard, and ``position`` reports it at once. Media that cannot
  /// seek plays from its start.
  @discardableResult
  public func open(
    _ url: String, headers: [String: String] = [:], start: TimeInterval = 0
  ) -> Bool {
    guard let lines = Self.headerLines(headers) else { return false }
    return lines.withCString { lines in
      var options = lychee_open_options()
      options.struct_size = UInt32(MemoryLayout<lychee_open_options>.size)
      options.headers = lines
      guard lychee_player_open_with_options(handle, url, &options) == 0 else {
        return false
      }
      // While loading, a seek sets where playback starts.
      if start > 0 {
        lychee_player_seek(handle, micros(start))
      }
      return true
    }
  }

  /// `headers` as the "Name: value" lines the C API takes, or nil if one
  /// would start another header.
  private static func headerLines(_ headers: [String: String]) -> String? {
    var lines = ""
    for (name, value) in headers {
      if name.isEmpty || name.contains(where: { $0.isNewline || $0 == ":" })
        || value.contains(where: \.isNewline)
      {
        return nil
      }
      lines += "\(name): \(value)\r\n"
    }
    return lines
  }

  /// Replaces what is open with `url`, the same content from elsewhere
  /// (another quality, a mirror, a fresh URL), where playback is if
  /// `keepPosition`, else from the start. Until the new media is ready the
  /// state is ``State/buffering``, and the position, duration, video size
  /// and the frame on screen stay; play/pause, volume, rate, looping and
  /// added subtitle files are kept. Media that cannot seek plays from its
  /// start.
  public func switchSource(_ url: String, keepPosition: Bool = true) {
    lychee_player_switch_source(handle, url, keepPosition ? 1 : 0)
  }

  /// Opens `url` in the background as the media to play when what is open
  /// ends, so that it starts without the wait of an open; `nil` unsets it.
  /// When the current media ends, ``Event/itemChanged(gapless:)`` says `url`
  /// is the media open, and the state does not become ``State/ended``.
  /// Where its default tracks continue the ones playing (the same codecs
  /// and configuration, video of the same size, audio of the same rate and
  /// channels) and both can seek, it plays on without a gap; otherwise the
  /// player switches to it at the end, buffering briefly. Looping replays
  /// the current media instead, and a seek before `url` starts plays on in
  /// the current media. ``open(_:start:)`` and ``close()`` unset it;
  /// ``switchSource(_:keepPosition:)`` keeps it.
  public func setNext(_ url: String?) {
    lychee_player_set_next(handle, url)
  }

  /// Stops what is open and goes back to ``State/idle``, releasing decoders,
  /// the audio output and the input. The player, its event handler, volume,
  /// rate, looping and play/pause stay for the next ``open(_:)``.
  public func close() {
    lychee_player_close(handle)
  }

  /// Plays from the start again if the state is ``State/ended``.
  public func play() {
    lychee_player_play(handle)
  }

  public func pause() {
    lychee_player_pause(handle)
  }

  /// Recreates video decoding after a platform invalidates it, keeping playback state.
  public func rebuildVideoDecoder() {
    lychee_player_rebuild_video_decoder(handle)
  }

  /// Ignored where ``isSeekable`` is false.
  public func seek(to position: TimeInterval) {
    lychee_player_seek(handle, micros(position))
  }

  /// 0 - 1. Kept across ``open(_:)``. Moves there over 10 ms, so the
  /// change does not click, and stops a ``fade(to:duration:)`` under way.
  /// After a fade, its target.
  public var volume: Float {
    get { volumeTarget }
    set {
      volumeTarget = newValue
      lychee_player_set_volume(handle, newValue)
    }
  }
  private var volumeTarget: Float = 1

  /// Moves ``volume`` to `volume` (0 - 1) over `duration` seconds of audio
  /// played, e.g. to fade a track out before the next one or in after it.
  /// Goes on from where a fade under way has got to, and holds while
  /// paused; an open meanwhile starts at `volume`.
  public func fade(to volume: Float, duration: TimeInterval) {
    volumeTarget = volume
    lychee_player_fade(handle, volume, micros(duration))
  }

  /// A gain in dB (-60 to +24, 0 by default) on top of ``volume``, such as
  /// a ReplayGain track or album gain. Kept across opens: set it for each
  /// input. A boost is lowered so the loudest sample stays within full
  /// scale: `peak`, the input's largest sample (1 at full scale), if
  /// known; otherwise the boost comes down when the decoder first meets a
  /// louder sample, ahead of it playing.
  public func setGain(_ gainDB: Float, peak: Float? = nil) {
    lychee_player_set_gain(handle, gainDB, peak ?? 0)
  }

  /// The last `frames` samples the audio output played by `displayTime`
  /// (`lychee_now_us()` timebase, now by default), mixed down to mono,
  /// after volume and gain, at ``Stats/audioOutputSampleRate``: for
  /// visualizers. The first call starts recording what plays, so it and
  /// any time the output played nothing read as silence; recording stops a
  /// second after the last call. Nil without an audio track.
  public func audioSamples(_ frames: Int, displayTime: Int64? = nil) -> [Float]? {
    guard frames > 0 else { return nil }
    var samples = [Float](repeating: 0, count: frames)
    let got = samples.withUnsafeMutableBufferPointer {
      lychee_player_get_audio_samples(
        handle, displayTime ?? lychee_now_us(), $0.baseAddress, Int32(clamping: frames))
    }
    return got > 0 ? samples : nil
  }

  /// Playback speed, clamped to 0.25 - 4. Audio keeps its pitch.
  public var rate: Double {
    get { lychee_player_get_rate(handle) }
    set { lychee_player_set_rate(handle, newValue) }
  }

  /// At the end, play again from the start instead of ending.
  public var isLooping: Bool {
    get { lychee_player_is_looping(handle) != 0 }
    set { lychee_player_set_looping(handle, newValue ? 1 : 0) }
  }

  /// Whether ``seek(to:)`` can move playback: false until
  /// ``Event/mediaInfo``, for live input and at ``State/error``. An http
  /// server without range requests cannot seek.
  public var isSeekable: Bool {
    lychee_player_is_seekable(handle) != 0
  }

  /// The positions ``seek(to:)`` reaches, nil if it cannot seek; the
  /// upper bound is nil if unknown. Live HLS whose playlist keeps a window
  /// counts positions from the start of the playlist's first segment when
  /// it was opened; the range moves on in real time, and playback starts
  /// at ``liveEdge``.
  public var seekableRange: (start: TimeInterval, end: TimeInterval?)? {
    var start: Int64 = 0
    var end: Int64 = 0
    guard lychee_player_get_seekable_range(handle, &start, &end) == 0 else { return nil }
    return (seconds(start), end < 0 ? nil : seconds(end))
  }

  /// Live HLS that can seek: the position that plays live, as it moves
  /// on; seek there to return to live. nil for other input.
  public var liveEdge: TimeInterval? {
    let edge = lychee_player_get_live_edge(handle)
    return edge < 0 ? nil : seconds(edge)
  }

  public var state: State {
    State(rawValue: lychee_player_get_state(handle)) ?? .idle
  }

  /// Whether playback is requested; stays true at ``State/ended``.
  public var isPlaying: Bool {
    lychee_player_is_playing(handle) != 0
  }

  public var position: TimeInterval {
    seconds(lychee_player_get_position(handle))
  }

  /// nil if unknown.
  public var duration: TimeInterval? {
    let duration = lychee_player_get_duration(handle)
    return duration < 0 ? nil : seconds(duration)
  }

  public var bufferedPosition: TimeInterval {
    seconds(lychee_player_get_buffered_position(handle))
  }

  /// Display size, before rotation; nil if there is no video.
  public var videoSize: CGSize? {
    var width: Int32 = 0
    var height: Int32 = 0
    guard lychee_player_get_video_size(handle, &width, &height) != 0 else { return nil }
    return CGSize(width: Int(width), height: Int(height))
  }

  /// Clockwise rotation in degrees: 0, 90, 180 or 270.
  public var rotation: Int {
    Int(lychee_player_get_video_rotation(handle))
  }

  /// The last error, once the state is ``State/error``.
  public var error: String? {
    guard state == .error else { return nil }
    return readString { lychee_player_get_error(handle, $0, $1) }
  }

  public var tracks: [Track] {
    (0..<lychee_player_get_track_count(handle)).compactMap { index in
      var info = lychee_track_info()
      info.struct_size = UInt32(MemoryLayout<lychee_track_info>.size)
      guard lychee_player_get_track_info(handle, index, &info) == 0,
            let type = TrackType(rawValue: info.type) else { return nil }
      return Track(
        index: Int(index), type: type, id: Int(info.id), isSelected: info.selected != 0,
        codec: string(&info.codec), language: string(&info.language),
        title: string(&info.title), width: Int(info.width), height: Int(info.height),
        sampleRate: Int(info.sample_rate), channels: Int(info.channels),
        isExternal: info.external != 0, isDefault: info.is_default != 0,
        isForced: info.forced != 0, role: TrackRole(rawValue: info.role) ?? .main,
        bitrate: max(info.bitrate, 0))
    }
  }

  /// Switches the `type` track to ``Track/index``, or disables `type` if
  /// `index` is nil. Returns false if the request was rejected;
  /// ``Event/tracksChanged`` follows otherwise.
  @discardableResult
  public func selectTrack(_ type: TrackType, index: Int?) -> Bool {
    lychee_player_select_track(handle, type.rawValue, Int32(index ?? -1)) == 0
  }

  /// The variants of an HLS master playlist, from ``Event/mediaInfo`` on;
  /// empty for other media.
  public var variants: [Variant] {
    (0..<lychee_player_get_variant_count(handle)).compactMap { index in
      var info = lychee_variant_info()
      info.struct_size = UInt32(MemoryLayout<lychee_variant_info>.size)
      guard lychee_player_get_variant_info(handle, index, &info) == 0 else { return nil }
      return Variant(
        index: Int(index), bandwidth: info.bandwidth, width: Int(info.width),
        height: Int(info.height), frameRate: info.frame_rate, codecs: string(&info.codecs),
        track: Int(info.track), isPlaying: info.playing != 0, isSelected: info.selected != 0)
    }
  }

  /// Plays ``Variant/index`` only, or adapts to the bandwidth again for
  /// nil. Returns false if the request was rejected;
  /// ``Event/variantChanged(_:)`` follows once it is on screen.
  @discardableResult
  public func selectVariant(_ index: Int?) -> Bool {
    lychee_player_select_variant(handle, Int32(index ?? -1)) == 0
  }

  /// The highest bandwidth (bit/s) automatic variant selection plays, or
  /// nil for no limit. Kept across ``open(_:)``.
  public var maxBitrate: Int64? {
    didSet { lychee_player_set_max_bitrate(handle, max(maxBitrate ?? 0, 0)) }
  }

  /// Reads the subtitle file `url` (a path, file:// or http(s):// URL;
  /// SubRip, WebVTT or ASS/SSA) in the background and adds it to the media
  /// opened last as a subtitle track after the container's, its cue times
  /// counting from the start of the media. Once loaded it is selected,
  /// unless `select` is false: then the subtitle track selected before (or
  /// none) stays. ``Event/subtitleAdded(id:added:)`` with the returned id
  /// says how it went. Opening other media drops it. Returns nil if nothing
  /// is open, or if `headers` (HTTP request headers for `url`, as in
  /// ``open(_:headers:)``; the media's are not used) holds a line break.
  ///
  /// Without a byte order mark, the text is in `encoding`, an IANA name
  /// such as "GB18030", "Big5", "Shift_JIS" or "windows-1251"; nil reads
  /// UTF-8, or guesses between GB18030 (GBK), Big5 and windows-1252 when
  /// the file is not UTF-8.
  @discardableResult
  public func addSubtitle(
    _ url: String, language: String? = nil, title: String? = nil, encoding: String? = nil,
    select: Bool = true, headers: [String: String] = [:]
  ) -> Int? {
    guard let lines = Self.headerLines(headers) else { return nil }
    let id = withOptionalCString(language) { language in
      withOptionalCString(title) { title in
        withOptionalCString(encoding) { encoding in
          lines.withCString { lines in
            var options = lychee_subtitle_options()
            options.struct_size = UInt32(MemoryLayout<lychee_subtitle_options>.size)
            options.language = language
            options.title = title
            options.encoding = encoding
            options.headers = lines
            options.keep_selection = select ? 0 : 1
            return lychee_player_add_subtitle_with_options(handle, url, &options)
          }
        }
      }
    }
    return id >= 0 ? Int(id) : nil
  }

  /// Shows the subtitles of every track this many seconds later; negative
  /// shows them earlier (a container's only as far as the read is ahead,
  /// usually a few seconds). Kept across ``open(_:)``.
  public var subtitleDelay: TimeInterval = 0 {
    didSet { lychee_player_set_subtitle_delay(handle, micros(subtitleDelay)) }
  }

  /// The subtitle text on screen now, lines separated by "\n".
  public var subtitle: String? {
    readString { lychee_player_get_subtitle(handle, 0, $0, $1) }
  }

  /// The subtitle pictures on screen now on a `width` x `height` canvas
  /// (0 x 0: the video's size before rotation); nil if there are none.
  /// Each call copies the pictures into new images, unless their
  /// ``SubtitlePictures/id`` is `unlessId`: then it returns that id with
  /// no pictures. Pass the id of the pictures you show to call it cheaply
  /// for every frame.
  public func subtitlePictures(
    width: Int = 0, height: Int = 0, unlessId: Int64? = nil
  ) -> SubtitlePictures? {
    guard let subtitle = lychee_player_acquire_subtitle(handle, 0, Int32(width), Int32(height)) else {
      return nil
    }
    defer { lychee_subtitle_release(subtitle) }
    let id = lychee_subtitle_get_id(subtitle)
    if id == unlessId { return SubtitlePictures(id: id, pictures: []) }
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
    let pictures = (0..<lychee_subtitle_get_image_count(subtitle)).compactMap {
      index -> (image: CGImage, rect: CGRect)? in
      var image = lychee_subtitle_image()
      image.struct_size = UInt32(MemoryLayout<lychee_subtitle_image>.size)
      guard lychee_subtitle_get_image(subtitle, index, &image) == 0, let data = image.data else {
        return nil
      }
      // Copied: the pixels go with the subtitle.
      let bytes = Data(bytes: data, count: Int(image.stride) * Int(image.bitmap_height))
      guard let provider = CGDataProvider(data: bytes as CFData),
            let cgImage = CGImage(
              width: Int(image.bitmap_width), height: Int(image.bitmap_height),
              bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(image.stride),
              space: space, bitmapInfo: info, provider: provider, decode: nil,
              shouldInterpolate: true, intent: .defaultIntent) else { return nil }
      let rect = CGRect(
        x: Int(image.x), y: Int(image.y), width: Int(image.width), height: Int(image.height))
      return (cgImage, rect)
    }
    return SubtitlePictures(id: id, pictures: pictures)
  }

  /// The container tag `key` ("title", "artist", ...; case-insensitive).
  public func metadata(_ key: String) -> String? {
    readString { lychee_player_get_metadata(handle, key, $0, $1) }
  }

  /// The cover picture as the media stores it, usually JPEG or PNG (for
  /// `NSImage(data:)` / `UIImage(data:)`); nil if it has none.
  public var artwork: Data? {
    let size = lychee_player_get_artwork(handle, nil, 0)
    if size < 0 { return nil }
    var data = Data(count: Int(size))
    let copied = data.withUnsafeMutableBytes {
      lychee_player_get_artwork(handle, $0.bindMemory(to: UInt8.self).baseAddress, size)
    }
    return data.prefix(Int(min(max(copied, 0), size)))
  }

  public var stats: Stats {
    var stats = lychee_stats()
    stats.struct_size = UInt32(MemoryLayout<lychee_stats>.size)
    lychee_player_get_stats(handle, &stats)
    return Stats(
      hardwareDecoding: stats.hardware_decoding != 0,
      framesRendered: Int(stats.frames_rendered),
      framesDropped: Int(stats.frames_dropped),
      videoDecoder: string(&stats.video_decoder),
      videoBuffered: seconds(stats.video_buffered_us),
      audioBuffered: seconds(stats.audio_buffered_us),
      videoDecodeTime: seconds(stats.video_decode_time_us),
      audioDecoder: string(&stats.audio_decoder),
      framesDecoded: Int(stats.frames_decoded),
      videoBytesRead: stats.video_bytes_read,
      audioBytesRead: stats.audio_bytes_read,
      downloadRate: stats.download_rate_bps >= 0 ? stats.download_rate_bps : nil,
      startupTime: seconds(stats.startup_time_us),
      rebufferCount: Int(stats.rebuffer_count),
      rebufferTime: seconds(stats.rebuffer_time_us),
      seekCount: Int(stats.seek_count),
      lastSeekTime: seconds(stats.last_seek_time_us),
      videoOffset: Double(stats.video_offset_us) / 1_000_000,
      audioOutputSampleRate: Int(stats.audio_output_sample_rate),
      audioOutputChannels: Int(stats.audio_output_channels),
      audioOutputLatency: seconds(stats.audio_output_latency_us),
      variantBandwidth: stats.variant_bandwidth_bps > 0 ? stats.variant_bandwidth_bps : nil)
  }

  fileprivate func event(_ type: Int32, _ arg0: Int64, _ arg1: Int64) -> Event? {
    switch lychee_event_type(rawValue: UInt32(type)) {
    case LYCHEE_EVENT_STATE_CHANGED:
      return State(rawValue: Int32(arg0)).map { .stateChanged($0) }
    case LYCHEE_EVENT_PLAYING_CHANGED: return .playingChanged(arg0 != 0)
    case LYCHEE_EVENT_MEDIA_INFO: return .mediaInfo
    case LYCHEE_EVENT_VIDEO_SIZE_CHANGED:
      return .videoSizeChanged(CGSize(width: Int(arg0), height: Int(arg1)))
    case LYCHEE_EVENT_SEEK_COMPLETED: return .seekCompleted(seconds(arg0))
    case LYCHEE_EVENT_ERROR: return .error
    case LYCHEE_EVENT_TRACKS_CHANGED: return .tracksChanged
    case LYCHEE_EVENT_SUBTITLE_CHANGED: return .subtitleChanged(pictures: arg1 != 0)
    case LYCHEE_EVENT_SUBTITLE_ADDED: return .subtitleAdded(id: Int(arg0), added: arg1 != 0)
    case LYCHEE_EVENT_VARIANT_CHANGED: return .variantChanged(Int(arg0))
    case LYCHEE_EVENT_ITEM_CHANGED: return .itemChanged(gapless: arg0 != 0)
    default: return nil
    }
  }
}

private final class EventSink {
  weak var player: LycheePlayer?

  // Main queue.
  func deliver(_ type: Int32, _ arg0: Int64, _ arg1: Int64) {
    guard let player, let event = player.event(type, arg0, arg1) else { return }
    player.onEventForView?(event)
    player.onEvent?(event)
  }
}

/// `body` with `text` as a C string, or with nil.
private func withOptionalCString<R>(
  _ text: String?, _ body: (UnsafePointer<CChar>?) -> R
) -> R {
  guard let text else { return body(nil) }
  return text.withCString { body($0) }
}

private func micros(_ seconds: TimeInterval) -> Int64 {
  Int64((seconds * 1e6).rounded())
}

private func seconds(_ micros: Int64) -> TimeInterval {
  TimeInterval(micros) / 1e6
}

// A NUL-terminated char array of a C struct.
private func string<T>(_ array: inout T) -> String {
  withUnsafeBytes(of: &array) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
}

// Calls `copy(buffer, size)`, which returns the full length (<= 0: none),
// again with a larger buffer if the first was too small.
private func readString(_ copy: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> String? {
  var size: Int32 = 256
  while true {
    var buffer = [CChar](repeating: 0, count: Int(size))
    let length = copy(&buffer, size)
    if length <= 0 { return nil }
    if length < size { return String(cString: buffer) }
    size = length + 1
  }
}
