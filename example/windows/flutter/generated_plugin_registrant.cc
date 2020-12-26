//
//  Generated file. Do not edit.
//

#include "generated_plugin_registrant.h"

#include <audio_player/audio_player_plugin.h>
#include <system_clock/system_clock_plugin.h>

void RegisterPlugins(flutter::PluginRegistry* registry) {
  AudioPlayerPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("AudioPlayerPlugin"));
  SystemClockPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("SystemClockPlugin"));
}
