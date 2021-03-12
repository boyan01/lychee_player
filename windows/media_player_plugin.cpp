#include "include/media_player/media_player_plugin.h"

void MediaPlayerPluginRegisterWithRegistrar(
        FlutterDesktopPluginRegistrarRef registrar) {
    register_flutter_plugin(
            flutter::PluginRegistrarManager::GetInstance()
                    ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
