//
// Created by boyan on 2021/2/14.
//

#if !defined(AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H) && defined(_FLUTTER_WINDOWS)
#define AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H

#include "flutter/texture_registrar.h"
#include "flutter/plugin_registrar_windows.h"

#include "ffp_define.h"
#include "ffp_player.h"
#include "render_video_flutter.h"

class FlutterWindowsVideoRender : public FlutterVideoRender {

 private:

  int64_t texture_id = -1;
  FlutterDesktopPixelBuffer *pixel_buffer = nullptr;
  std::unique_ptr<flutter::TextureVariant> texture_;
  struct SwsContext *img_convert_ctx = nullptr;

 public:

  void RenderPicture(Frame &frame) override;

  int Attach();

  void Detach();

  ~FlutterWindowsVideoRender();
};


/**
 * Register windows flutter plugin.
 */
FFPLAYER_EXPORT void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar);

#endif //AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H
