#ifndef FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_
#define FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <map>
#include <memory>

#include "lychee_texture.h"

namespace lychee_flutter {

// The "lychee_player" method channel: createTexture({player}) -> textureId,
// disposeTexture({textureId}) and disposeAll(). Dart owns the players; the
// plugin only shows them, and destroys them once Dart is gone (disposeAll
// after a hot restart, or the engine shutting down).
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
  void DisposeAll();

  struct Entry {
    std::unique_ptr<LycheeTexture> texture;
    // The player Dart created the texture for.
    lychee_player* player;
  };

  flutter::PluginRegistrarWindows* const registrar_;
  std::map<int64_t, Entry> textures_;
};

}  // namespace lychee_flutter

#endif  // FLUTTER_PLUGIN_LYCHEE_PLAYER_PLUGIN_H_
