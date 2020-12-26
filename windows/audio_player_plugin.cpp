#include "include/audio_player/audio_player_plugin.h"
#include "include/audio_player/client_audio_player.h"
// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
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

        MethodChannel<EncodableValue> *methodChannel{};

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
                std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
                        registrar->messenger(), "tech.soit.flutter.audio_player",
                        &flutter::StandardMethodCodec::GetInstance());

        auto plugin = std::make_unique<AudioPlayerPlugin>();

        channel->SetMethodCallHandler(
                [plugin_pointer = plugin.get()](const auto &call, auto result) {
                    plugin_pointer->HandleMethodCall(call, std::move(result));
                });
        plugin->methodChannel = channel.get();

        registrar->AddPlugin(std::move(plugin));
    }


    template<class T>
    T getArgument(const EncodableMap *params, const char *key) {
        auto pair = params->find(EncodableValue(key));
        if (pair == params->end()) {
            return nullptr;
        }
        return get<T>(pair->second);
    }

    AudioPlayerPlugin::~AudioPlayerPlugin() = default;

    void AudioPlayerPlugin::HandleMethodCall(
            const MethodCall<EncodableValue> &methodCall,
            std::unique_ptr<MethodResult<EncodableValue>> result) {
        std::cout << "hand call: " << methodCall.method_name() << " " << methodCall.arguments() << std::endl;

        if (methodCall.method_name() == "initialize") {
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
        if (methodCall.method_name() == "create") {
            if (players[playerId] != nullptr) {
                result->Error("1", "player already created");
            } else {
                auto url = getArgument<string>(arguments, "url");
                auto player = new ClientAudioPlayer(wstring(url.begin(), url.end()).c_str(), methodChannel, playerId);
                players[playerId] = player;
                result->Success();
            }
        }

        if (methodCall.method_name() == "getPlatformVersion") {
            std::ostringstream version_stream;
            version_stream << "Windows ";
            if (IsWindows10OrGreater()) {
                version_stream << "10+";
            } else if (IsWindows8OrGreater()) {
                version_stream << "8";
            } else if (IsWindows7OrGreater()) {
                version_stream << "7";
            }
            result->Success(flutter::EncodableValue(version_stream.str()));
        } else {
            result->NotImplemented();
        }
    }

    AudioPlayerPlugin::AudioPlayerPlugin() = default;

} //namespace

void AudioPlayerPluginRegisterWithRegistrar(
        FlutterDesktopPluginRegistrarRef registrar) {
    AudioPlayerPlugin::RegisterWithRegistrar(
            flutter::PluginRegistrarManager::GetInstance()
                    ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

