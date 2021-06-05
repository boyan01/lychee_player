//
//  Generated file. Do not edit.
//

#include "generated_plugin_registrant.h"

#include <lychee_player/lychee_player_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) lychee_player_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "LycheePlayerPlugin");
  lychee_player_plugin_register_with_registrar(lychee_player_registrar);
}
