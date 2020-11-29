import AVKit
import Flutter
import UIKit

public class SwiftAudioPlayerPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: "tech.soit.flutter.audio_player", binaryMessenger: registrar.messenger())
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
            guard let assetKey = registrar?.lookupKey(forAsset: urlString) else { return nil }
            guard let path = Bundle.main.path(forResource: assetKey, ofType: nil) else { return nil }
            return URL(fileURLWithPath: path)
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
            NotificationCenter.default.addObserver(self, selector: #selector(onPlayerEnded), name: .AVPlayerItemDidPlayToEndTime, object: nil)
            player.actionAtItemEnd = .none
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
            let params: [String: Any] = ["position": player.currentTime().inMills, "updateTime": systemUptime()]
            switch self.player.timeControlStatus {
            case .paused:
                dispatchEvent(.paused, params: params)
            case .playing:
                dispatchEvent(.playing, params: params)
            case .waitingToPlayAtSpecifiedRate:
                dispatchEvent(.buffering, params: params)
            @unknown default:
                break
            }
        }

        if keyPath == #keyPath(AVPlayerItem.status), let playItem = playItem {
            if playItem.status == .readyToPlay {
                dispatchEvent(.prepared, params: ["duration": playItem.duration.inMills])
            }
        }
    }
}

enum PlaybackEvent: Int {
    case paused
    case playing
    case preparing
    case prepared
    case buffering
    case error
    case seeking
    case seekFinished
    case end
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
