#include "include/audio_player/audio_player_plugin.h"

#include "include/audio_player/client_audio_player.h"
// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include "ffplayer/flutter.h"
#include <mfplay.h>

#include <map>
#include <memory>
#include <sstream>

using namespace std;

using namespace flutter;


namespace {

    class AudioPlayerPlugin : public flutter::Plugin {
    public:
        static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

        AudioPlayerPlugin();

        ~AudioPlayerPlugin() override;

    private:
        shared_ptr<MethodChannel<EncodableValue>> methodChannel;

        map<string, ClientAudioPlayer *> players = map<string, ClientAudioPlayer *>();

        // Called when a method is called on this plugin's channel from Dart.
        void HandleMethodCall(
                const flutter::MethodCall<EncodableValue> &methodCall,
                std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    };

// static
    void AudioPlayerPlugin::RegisterWithRegistrar(
            flutter::PluginRegistrarWindows *registrar) {
        auto channel =
                std::make_shared<flutter::MethodChannel<flutter::EncodableValue >>(
                        registrar->messenger(), "tech.soit.flutter.audio_player",
                        &flutter::StandardMethodCodec::GetInstance());

        auto plugin = std::make_unique<AudioPlayerPlugin>();

        register_flutter_plugin(registrar);

        channel->SetMethodCallHandler(
                [plugin_pointer = plugin.get()](const auto &call, auto result) {
                    plugin_pointer->HandleMethodCall(call, std::move(result));
                });
        plugin->methodChannel = channel;
        registrar->AddPlugin(std::move(plugin));
    }

    template<class T>
    T getArgument(const EncodableMap *params, const char *key) {
        auto pair = params->find(EncodableValue(key));
        if (pair == params->end()) {
            throw exception("error");
        }
        return get<T>(pair->second);
    }

    AudioPlayerPlugin::~AudioPlayerPlugin() = default;

    AudioPlayerPlugin::AudioPlayerPlugin() = default;

    void debugPrintMethodCall(const MethodCall<EncodableValue> &methodCall) {
        std::cout << "hand call: " << methodCall.method_name() << " args = ";
        auto argMap = get_if<EncodableMap>(methodCall.arguments());
        if (argMap) {
            for (auto pair : *argMap) {
                cout << get<string>(pair.first) << ":";
                if (holds_alternative<string>(pair.second)) {
                    cout << get<string>(pair.second);
                } else if (holds_alternative<int32_t>(pair.second)) {
                    cout << get<int32_t>(pair.second);
                } else if (holds_alternative<int64_t>(pair.second)) {
                    cout << get<int64_t>(pair.second);
                } else if (holds_alternative<double>(pair.second)) {
                    cout << get<double>(pair.second);
                } else if (holds_alternative<bool>(pair.second)) {
                    cout << get<bool>(pair.second);
                } else {
                    cout << "CAN NOT PARSE";
                }
                cout << " ";
            }
        } else {
            cout << "nil";
        }
        cout << std::endl;
    }

    void AudioPlayerPlugin::HandleMethodCall(
            const MethodCall<EncodableValue> &methodCall,
            std::unique_ptr<MethodResult<EncodableValue>> result) {
        debugPrintMethodCall(methodCall);

        if (methodCall.method_name() == "initialize") {
            for (const auto &pair: players) {
                delete pair.second;
            }
            players.clear();
            result->Success();
            return;
        }

        const EncodableMap *arguments = std::get_if<EncodableMap>(methodCall.arguments());
        auto playerId = getArgument<string>(arguments, "playerId");
        if (playerId.empty()) {
            result->Error("can not find playerId");
            return;
        }

        auto answerWithPlayer = [playerId, this, result = result.get()](function<void(ClientAudioPlayer *)> action) {
            auto player = this->players[playerId];
            if (player) {
                action(player);
                result->Success();
            } else {
                result->Error("2", "player haven't created");
            }
        };

        if (methodCall.method_name() == "create") {
            if (players[playerId] != nullptr) {
                result->Error("1", "player already created");
            } else {
                auto url = getArgument<string>(arguments, "url");
                auto player = new ClientAudioPlayer(wstring(url.begin(), url.end()).c_str(), methodChannel,
                                                    playerId.c_str());
                printf("create player: %s,  %p \n", playerId.c_str(), player);
                players[playerId] = player;
                result->Success();
            }
        } else if (methodCall.method_name() == "play") {
            answerWithPlayer([](auto player) {
                player->play();
            });
        } else if (methodCall.method_name() == "pause") {
            answerWithPlayer([](auto player) {
                player->pause();
            });
        } else if (methodCall.method_name() == "seek") {
            answerWithPlayer([arguments](ClientAudioPlayer *player) {
                auto position = arguments->find(EncodableValue("position"))->second;
                player->seekTo(position.LongValue());
            });
        } else if (methodCall.method_name() == "dispose") {
            answerWithPlayer([playerId, this](auto player) {
                this->players.erase(playerId);
                delete player;
            });
        } else {
            result->NotImplemented();
        }
    }

}  //namespace

void AudioPlayerPluginRegisterWithRegistrar(
        FlutterDesktopPluginRegistrarRef registrar) {
    AudioPlayerPlugin::RegisterWithRegistrar(
            flutter::PluginRegistrarManager::GetInstance()
                    ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
