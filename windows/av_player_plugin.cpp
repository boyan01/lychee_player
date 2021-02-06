#include "include/av_player/av_player_plugin.h"

void AvPlayerPluginRegisterWithRegistrar(
        FlutterDesktopPluginRegistrarRef registrar) {
    register_flutter_plugin(
            flutter::PluginRegistrarManager::GetInstance()
                    ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
