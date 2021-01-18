//
//  Generated file. Do not edit.
//

#include "generated_plugin_registrant.h"

#include <audio_player/audio_player_plugin.h>
#include <system_clock/system_clock_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) audio_player_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "AudioPlayerPlugin");
  audio_player_plugin_register_with_registrar(audio_player_registrar);
  g_autoptr(FlPluginRegistrar) system_clock_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "SystemClockPlugin");
  system_clock_plugin_register_with_registrar(system_clock_registrar);
}
