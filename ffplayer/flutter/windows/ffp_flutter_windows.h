//
// Created by boyan on 2021/2/14.
//

#if !defined(AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H)
#define AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H

#include <cstdint>

#include "flutter/texture_registrar.h"
#include "flutter/plugin_registrar_windows.h"

#include "ffp_define.h"
#include "media_player.h"
#include "render_video_flutter.h"
#include "audio_render_sdl.h"

class FlutterWindowsVideoRender : public FlutterVideoRender {

 private:

  int64_t texture_id_ = -1;
  FlutterDesktopPixelBuffer *pixel_buffer_ = nullptr;
  std::unique_ptr<flutter::TextureVariant> texture_;
  struct SwsContext *img_convert_ctx_ = nullptr;

 public:

  void RenderPicture(Frame &frame) override;

  int64_t Attach();

  void Detach();

  ~FlutterWindowsVideoRender();
};


/**
 * Register windows flutter plugin.
 */
FFPLAYER_EXPORT void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar);

#endif //AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H
