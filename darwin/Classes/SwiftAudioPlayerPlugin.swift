import AVKit
#if os(iOS)
    import Flutter
    import UIKit
#elseif os(macOS)
    import FlutterMacOS
#endif

public class SwiftAudioPlayerPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        #if os(iOS)
            let channel = FlutterMethodChannel(name: "tech.soit.flutter.audio_player", binaryMessenger: registrar.messenger())
        #elseif os(macOS)
            let channel = FlutterMethodChannel(name: "tech.soit.flutter.audio_player", binaryMessenger: registrar.messenger)
        #endif
        let instance = SwiftAudioPlayerPlugin(channel: channel)
        instance.registrar = registrar
        registrar.addMethodCallDelegate(instance, channel: channel)
    }

    init(channel: FlutterMethodChannel) {
        self.channel = channel
        super.init()
    }

    private let channel: FlutterMethodChannel

    private weak var registrar: FlutterPluginRegistrar?

    private var players: [String: ClientAudioPlayer] = [:]

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        #if DEBUG
            debugPrint("invoke: \(call.method) args = \(call.arguments ?? "empty")")
        #endif

        if call.method == "initialize" {
            players.removeAll()
            result(nil)
            return
        }

        let playerId: String = call.argument(key: "playerId")
        switch call.method {
        case "create":
            if players.keys.contains(playerId) {
                result(FlutterError.kPlayerAlreadyCreated)
            } else {
                let player = ClientAudioPlayer(url: parseURLForCreate(call: call), channel: channel, playerId: playerId)
                players[playerId] = player
                result(nil)
            }
        case "play":
            if let player = players[playerId] {
                player.play()
                result(nil)
            } else {
                result(FlutterError.kPlayerNotCreated)
            }
        case "pause":
            if let player = players[playerId] {
                player.pause()
                result(nil)
            } else {
                result(FlutterError.kPlayerNotCreated)
            }
        case "seek":
            if let player = players[playerId] {
                let position: Int = call.argument(key: "position")
                player.seek(to: Double(position) / 1000.0)
                result(nil)
            } else {
                result(FlutterError.kPlayerNotCreated)
            }
        case "dispose":
            players[playerId] = nil
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
        players.removeAll()
    }
}

extension SwiftAudioPlayerPlugin {
    func parseURLForCreate(call: FlutterMethodCall) -> URL? {
        let typeInt: Int = call.argument(key: "type")
        guard let type = DataSourceType(rawValue: typeInt) else { return nil }
        let urlString: String = call.argument(key: "url")
        switch type {
        case .asset:
            #if os(iOS)
                guard let assetKey = registrar?.lookupKey(forAsset: urlString) else { return nil }
                guard let path = Bundle.main.path(forResource: assetKey, ofType: nil) else { return nil }
                return URL(fileURLWithPath: path)
            #elseif os(macOS)
                // Do not support yet.
                return nil
            #endif
        case .url:
            return URL(string: urlString)
        case .file:
            return URL(fileURLWithPath: urlString)
        }
    }
}

extension FlutterError {
    public static let kPlayerAlreadyCreated = FlutterError(code: "1", message: "player already created.", details: nil)
    public static let kPlayerNotCreated = FlutterError(code: "2", message: "player haven't created.", details: nil)
}

extension FlutterMethodCall {
    func arguments<T>() -> T {
        arguments as! T
    }

    func argument<T>(key name: String) -> T {
        let arguments = self.arguments as! [String: AnyObject?]
        return arguments[name] as! T
    }
}

class ClientAudioPlayer: NSObject {
    private let player: AVPlayer

    private let channel: FlutterMethodChannel
    private let playerId: String

    private var playItem: AVPlayerItem?

    private var isWaitingToPlay: Bool = false

    private var isPlayEnded: Bool = false

    init(url: URL?, channel: FlutterMethodChannel, playerId: String) {
        self.channel = channel
        self.playerId = playerId

        let playItem: AVPlayerItem?
        if let url = url {
            playItem = AVPlayerItem(url: url)
        } else {
            playItem = nil
        }
        player = AVPlayer(playerItem: playItem)
        super.init()

        dispatchEvent(.preparing)

        if playItem == nil {
            dispatchEvent(.error)
        } else {
            player.addObserver(self, forKeyPath: #keyPath(AVPlayer.status), options: [.new], context: nil)
            player.addObserver(self, forKeyPath: #keyPath(AVPlayer.rate), options: [.new], context: nil)
            player.addObserver(self, forKeyPath: #keyPath(AVPlayer.timeControlStatus), options: [.new], context: nil)
            playItem?.addObserver(self, forKeyPath: #keyPath(AVPlayerItem.status), options: [.new], context: nil)
            playItem?.addObserver(self, forKeyPath: #keyPath(AVPlayerItem.loadedTimeRanges), options: [.new], context: nil)
            NotificationCenter.default.addObserver(self, selector: #selector(onPlayerEnded), name: .AVPlayerItemDidPlayToEndTime, object: nil)
            player.actionAtItemEnd = .pause
            self.playItem = playItem
        }
    }

    func play() {
        player.play()
    }

    func pause() {
        player.pause()
    }

    @objc func onPlayerEnded() {
        isPlayEnded = true
        dispatchEvent(.end, params: [
            "position": player.currentTime().inMills,
            "updateTime": systemUptime()])
    }

    func seek(to time: TimeInterval) {
        let targetTime = CMTime(seconds: time, preferredTimescale: 1000)
        dispatchEvent(.seeking, params: ["targetTime": targetTime.inMills])
        player.seek(to: targetTime) { finished in
            self.dispatchEvent(.seekFinished, params: [
                "position": self.player.currentTime().inMills,
                "updateTime": systemUptime(),
                "finished": finished,
            ])
            if self.isPlayEnded {
                self.isPlayEnded = false
                self.dispatchEvent(.buffering, params: [
                    "position": self.player.currentTime().inMills,
                    "updateTime": systemUptime()])
                self.dispatchEvent(.bufferingEnd, params: [
                    "position": self.player.currentTime().inMills,
                    "updateTime": systemUptime()])
            }
        }
    }

    private func dispatchEvent(_ event: PlaybackEvent, params: [String: Any?] = [:]) {
        var params = params
        params["playerId"] = playerId
        params["event"] = event.rawValue
        channel.invokeMethod("onPlaybackEvent", arguments: params)
    }

    override func observeValue(forKeyPath keyPath: String?, of object: Any?, change: [NSKeyValueChangeKey: Any]?, context: UnsafeMutableRawPointer?) {
        if keyPath == #keyPath(AVPlayer.status) {
            debugPrint("playback status \(String(describing: self.player.status.rawValue))")
            if player.status == .failed {
                dispatchEvent(.error)
            }
        } else if keyPath == #keyPath(AVPlayer.rate) {
            debugPrint("playback rate \(self.player.rate)")
        } else if keyPath == #keyPath(AVPlayer.timeControlStatus) {
            dispatchPlayerTimeControlStatus()

            let playing = player.timeControlStatus == .playing
            dispatchEvent(
                .onIsPlayingChanged,
                params: [
                    "playing": playing,
                    "position": self.player.currentTime().inMills,
                    "updateTime": systemUptime(),
                ])
        }

        if keyPath == #keyPath(AVPlayerItem.status), let playItem = playItem {
            if playItem.status == .readyToPlay {
                dispatchEvent(.prepared, params: ["duration": playItem.duration.inMills])
            }
        } else if keyPath == #keyPath(AVPlayerItem.loadedTimeRanges), let playItem = playItem {
            var ranges = [Int64]()
            playItem.loadedTimeRanges.forEach { value in
                let range = value.timeRangeValue
                ranges.append(range.start.inMills)
                ranges.append(range.end.inMills)
            }
            dispatchEvent(.updateBufferPosition, params: ["ranges": ranges])
        }
    }

    private func dispatchPlayerTimeControlStatus() {
        debugPrint("time control status \(String(describing: player.timeControlStatus.rawValue))")
        let params: [String: Any] = ["position": player.currentTime().inMills, "updateTime": systemUptime()]
        switch player.timeControlStatus {
        case .paused:
            dispatchEvent(.paused, params: params)
        case .playing:
            if isWaitingToPlay {
                isWaitingToPlay = false
                dispatchEvent(.bufferingEnd, params: params)
            }
            dispatchEvent(.playing, params: params)
        case .waitingToPlayAtSpecifiedRate:
            isWaitingToPlay = true
            dispatchEvent(.buffering, params: params)
        @unknown default:
            break
        }
    }
}

enum PlaybackEvent: Int {
    case paused
    case playing
    case preparing
    case prepared
    case buffering
    case bufferingEnd
    case error
    case seeking
    case seekFinished
    case end
    case updateBufferPosition
    case onIsPlayingChanged
}

enum DataSourceType: Int {
    case url
    case file
    case asset
}

extension CMTime {
    var inMills: Int64 {
        if timescale == 0 {
            return 0
        }
        return value * 1000 / Int64(timescale)
    }
}

func systemUptime() -> Int {
    var spec = timespec()
    clock_gettime(CLOCK_UPTIME_RAW, &spec)
    return spec.tv_sec * 1000 + spec.tv_nsec / 1000000
}
