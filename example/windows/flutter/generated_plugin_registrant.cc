//
//  Generated file. Do not edit.
//

#include "generated_plugin_registrant.h"

#include <media_player/media_player_plugin.h>
#include <system_clock/system_clock_plugin.h>

void RegisterPlugins(flutter::PluginRegistry* registry) {
  MediaPlayerPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("MediaPlayerPlugin"));
  SystemClockPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("SystemClockPlugin"));
}
