import AVKit
import Flutter
import UIKit

public class SwiftAudioPlayerPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: "tech.soit.flutter.audio_player", binaryMessenger: registrar.messenger())
        let instance = SwiftAudioPlayerPlugin(channel: channel)
        registrar.addMethodCallDelegate(instance, channel: channel)
    }

    init(channel: FlutterMethodChannel) {
        self.channel = channel
        super.init()
    }

    private let channel: FlutterMethodChannel

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
                let player = ClientAudioPlayer(url: call.argument(key: "url"), channel: channel, playerId: playerId)
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
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
        players.removeAll()
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

    init(url: String, channel: FlutterMethodChannel, playerId: String) {
        self.channel = channel
        self.playerId = playerId
        let playItem = AVPlayerItem(url: URL(string: url)!)
        player = AVPlayer(playerItem: playItem)
        super.init()
        player.addObserver(self, forKeyPath: #keyPath(AVPlayer.status), options: [.new], context: nil)
        player.addObserver(self, forKeyPath: #keyPath(AVPlayer.rate), options: [.new], context: nil)
        player.addObserver(self, forKeyPath: #keyPath(AVPlayer.timeControlStatus), options: [.new], context: nil)
        playItem.addObserver(self, forKeyPath: #keyPath(AVPlayerItem.status), options: [.new], context: nil)
        self.playItem = playItem
        dispatchEvent(.preparing)
    }

    func play() {
        player.play()
    }

    func pause() {
        player.pause()
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
