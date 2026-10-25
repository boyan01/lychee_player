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
    /// ``subtitle`` has the new text.
    case subtitleChanged
  }

  /// An audio, video or subtitle stream of the open media.
  public struct Track {
    /// Pass to ``selectTrack(_:index:)``.
    public let index: Int
    public let type: TrackType
    /// The container's stream index.
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
  }

  public struct Stats {
    public let hardwareDecoding: Bool
    public let framesRendered: Int
    public let framesDropped: Int
    public let videoDecoder: String
    /// Media read ahead of the playback position.
    public let videoBuffered: TimeInterval
    public let audioBuffered: TimeInterval
    /// Average time decoding one video packet takes.
    public let videoDecodeTime: TimeInterval
  }

  /// Called on the main queue.
  public var onEvent: ((Event) -> Void)?

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
    lychee_config_init(&config)
    config.video_frame_types = video ? LYCHEE_FRAME_CVPIXELBUFFER.rawValue : 0
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
  public func open(_ url: String) {
    lychee_player_open(handle, url)
  }

  /// Plays from the start again if the state is ``State/ended``.
  public func play() {
    lychee_player_play(handle)
  }

  public func pause() {
    lychee_player_pause(handle)
  }

  public func seek(to position: TimeInterval) {
    lychee_player_seek(handle, micros(position))
  }

  /// 0 - 1.
  public var volume: Float = 1 {
    didSet { lychee_player_set_volume(handle, volume) }
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
        sampleRate: Int(info.sample_rate), channels: Int(info.channels))
    }
  }

  /// Switches the `type` track to ``Track/index``, or disables `type` if
  /// `index` is nil. Returns false if the request was rejected;
  /// ``Event/tracksChanged`` follows otherwise.
  @discardableResult
  public func selectTrack(_ type: TrackType, index: Int?) -> Bool {
    lychee_player_select_track(handle, type.rawValue, Int32(index ?? -1)) == 0
  }

  /// The subtitle text on screen now, lines separated by "\n".
  public var subtitle: String? {
    readString { lychee_player_get_subtitle(handle, 0, $0, $1) }
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
      videoDecodeTime: seconds(stats.video_decode_time_us))
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
    case LYCHEE_EVENT_SUBTITLE_CHANGED: return .subtitleChanged
    default: return nil
    }
  }
}

private final class EventSink {
  weak var player: LycheePlayer?

  // Main queue.
  func deliver(_ type: Int32, _ arg0: Int64, _ arg1: Int64) {
    guard let player, let event = player.event(type, arg0, arg1) else { return }
    player.onEvent?(event)
  }
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
