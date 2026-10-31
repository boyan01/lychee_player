#include "lychee_player_plugin.h"

#include <dxgi.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>
#include <wrl/client.h>

#include <optional>

#include "include/lychee_player/lychee_host_texture.h"

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

// For players whose Dart side is gone: their event callback points into a
// dead isolate.
void DestroyOrphan(lychee_player* player) {
  lychee_player_set_event_callback(player, nullptr, nullptr);
  lychee_player_destroy(player);
}

// A texture for |registrar|'s view, showing nothing yet.
std::unique_ptr<LycheeTexture> CreateTexture(
    flutter::PluginRegistrarWindows* registrar) {
  // The view is looked up now, not at registration: it may not exist yet.
  flutter::FlutterView* view = registrar->GetView();
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (view) {
    adapter.Attach(view->GetGraphicsAdapter());
  }
  return std::make_unique<LycheeTexture>(
      registrar->texture_registrar(), view ? view->GetNativeWindow() : nullptr,
      adapter.Get());
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

// The engine is shutting down: stop the vsync threads and destroy the
// players, but leave the textures alive, since the engine may not run an
// unregister callback now.
LycheePlayerPlugin::~LycheePlayerPlugin() {
  for (auto& entry : textures_) {
    entry.second.texture->Dispose();
    DestroyOrphan(entry.second.player);
    (void)entry.second.texture.release();
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
    auto* created =
        reinterpret_cast<lychee_player*>(static_cast<intptr_t>(*player));
    std::unique_ptr<LycheeTexture> texture = CreateTexture(registrar_);
    texture->SetPlayer(created);
    const int64_t texture_id = texture->Start();
    textures_[texture_id] = {std::move(texture), created};
    result->Success(flutter::EncodableValue(texture_id));
  } else if (call.method_name() == "disposeAll") {
    DisposeAll();
    result->Success();
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
  it->second.texture.release()->Retire();
  textures_.erase(it);
}

void LycheePlayerPlugin::DisposeAll() {
  while (!textures_.empty()) {
    const int64_t texture_id = textures_.begin()->first;
    lychee_player* player = textures_.begin()->second.player;
    DisposeTexture(texture_id);
    DestroyOrphan(player);
  }
}

}  // namespace lychee_flutter

uint32_t LycheeHostTextureFrameTypes(void) {
  // ANGLE opens shared textures only in BGRA.
  return LYCHEE_FRAME_D3D11_TEXTURE | LYCHEE_FRAME_CPU_BGRA;
}

LycheeHostTexture* LycheeHostTextureCreate(
    FlutterDesktopPluginRegistrarRef registrar) {
  std::unique_ptr<lychee_flutter::LycheeTexture> texture =
      lychee_flutter::CreateTexture(
          flutter::PluginRegistrarManager::GetInstance()
              ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
  texture->Start();
  return reinterpret_cast<LycheeHostTexture*>(texture.release());
}

int64_t LycheeHostTextureGetId(LycheeHostTexture* texture) {
  return reinterpret_cast<lychee_flutter::LycheeTexture*>(texture)->id();
}

void LycheeHostTextureSetPlayer(LycheeHostTexture* texture,
                                lychee_player* player) {
  reinterpret_cast<lychee_flutter::LycheeTexture*>(texture)->SetPlayer(player);
}

void LycheeHostTextureDestroy(LycheeHostTexture* texture) {
  reinterpret_cast<lychee_flutter::LycheeTexture*>(texture)->Retire();
}
