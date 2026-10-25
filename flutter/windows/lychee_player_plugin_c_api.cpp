#include "include/lychee_player/lychee_player_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "lychee_player_plugin.h"

void LycheePlayerPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  lychee_flutter::LycheePlayerPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
