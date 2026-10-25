#ifndef FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_
#define FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <map>
#include <memory>

#include "lychee_texture.h"

namespace lychee_flutter {

// The "lychee_player" method channel: createTexture({player}) -> textureId
// and disposeTexture({textureId}). Dart owns the players; the plugin only
// shows them.
class LycheePlayerPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  explicit LycheePlayerPlugin(flutter::PluginRegistrarWindows* registrar);
  ~LycheePlayerPlugin() override;

  LycheePlayerPlugin(const LycheePlayerPlugin&) = delete;
  LycheePlayerPlugin& operator=(const LycheePlayerPlugin&) = delete;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void DisposeTexture(int64_t texture_id);

  flutter::PluginRegistrarWindows* const registrar_;
  std::map<int64_t, std::unique_ptr<LycheeTexture>> textures_;
};

}  // namespace lychee_flutter

#endif  // FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_
