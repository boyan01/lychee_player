//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <lychee_player/media_player_plugin.h>
#include <system_clock/system_clock_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) lychee_player_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "MediaPlayerPlugin");
  media_player_plugin_register_with_registrar(lychee_player_registrar);
  g_autoptr(FlPluginRegistrar) system_clock_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "SystemClockPlugin");
  system_clock_plugin_register_with_registrar(system_clock_registrar);
}
