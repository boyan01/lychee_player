//
//  Generated file. Do not edit.
//

#include "generated_plugin_registrant.h"

#include <av_player/av_player_plugin.h>
#include <system_clock/system_clock_plugin.h>

void RegisterPlugins(flutter::PluginRegistry* registry) {
  AvPlayerPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("AvPlayerPlugin"));
  SystemClockPluginRegisterWithRegistrar(
      registry->GetRegistrarForPlugin("SystemClockPlugin"));
}
