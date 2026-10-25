#include "lychee_player_plugin.h"

#include <dxgi.h>
#include <flutter/standard_method_codec.h>
#include <wrl/client.h>

#include <optional>

namespace lychee_flutter {

namespace {

std::optional<int64_t> GetInt(const flutter::EncodableValue* arguments,
                              const char* key) {
  const auto* map = std::get_if<flutter::EncodableMap>(arguments);
  if (!map) {
    return std::nullopt;
  }
  auto it = map->find(flutter::EncodableValue(key));
  if (it == map->end() || !(std::holds_alternative<int32_t>(it->second) ||
                            std::holds_alternative<int64_t>(it->second))) {
    return std::nullopt;
  }
  return it->second.LongValue();
}

}  // namespace

// static
void LycheePlayerPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "lychee_player",
          &flutter::StandardMethodCodec::GetInstance());
  auto plugin = std::make_unique<LycheePlayerPlugin>(registrar);
  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });
  registrar->AddPlugin(std::move(plugin));
}

LycheePlayerPlugin::LycheePlayerPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar) {}

// The engine is shutting down: stop the vsync threads, but leave the
// textures alive, since the engine may not run an unregister callback now.
LycheePlayerPlugin::~LycheePlayerPlugin() {
  for (auto& entry : textures_) {
    entry.second->Dispose();
    (void)entry.second.release();
  }
}

void LycheePlayerPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (call.method_name() == "createTexture") {
    const std::optional<int64_t> player = GetInt(call.arguments(), "player");
    if (!player || *player == 0) {
      result->Error("bad_args", "createTexture needs a player");
      return;
    }
    // The view is looked up now, not at registration: it may not exist yet.
    flutter::FlutterView* view = registrar_->GetView();
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (view) {
      adapter.Attach(view->GetGraphicsAdapter());
    }
    auto texture = std::make_unique<LycheeTexture>(
        reinterpret_cast<lychee_player*>(static_cast<intptr_t>(*player)),
        registrar_->texture_registrar(),
        view ? view->GetNativeWindow() : nullptr, adapter.Get());
    const int64_t texture_id = texture->Start();
    textures_[texture_id] = std::move(texture);
    result->Success(flutter::EncodableValue(texture_id));
  } else if (call.method_name() == "disposeTexture") {
    const std::optional<int64_t> texture_id =
        GetInt(call.arguments(), "textureId");
    if (texture_id) {
      DisposeTexture(*texture_id);
    }
    result->Success();
  } else {
    result->NotImplemented();
  }
}

void LycheePlayerPlugin::DisposeTexture(int64_t texture_id) {
  auto it = textures_.find(texture_id);
  if (it == textures_.end()) {
    return;
  }
  // Dart destroys the player as soon as this returns.
  it->second->Dispose();
  // The raster thread may still call the texture's pixel buffer callback
  // until Flutter has unregistered it.
  LycheeTexture* texture = it->second.release();
  textures_.erase(it);
  registrar_->texture_registrar()->UnregisterTexture(
      texture_id, [texture] { delete texture; });
}

}  // namespace lychee_flutter
