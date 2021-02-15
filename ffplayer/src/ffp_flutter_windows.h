//
// Created by boyan on 2021/2/14.
//

#if !defined(AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H) && defined(_FLUTTER_WINDOWS)
#define AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H

#include "flutter/texture_registrar.h"
#include "flutter/plugin_registrar_windows.h"

#include "ffp_define.h"
#include "ffp_player.h"

FFPLAYER_EXPORT void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar);

FFPLAYER_EXPORT int64_t flutter_attach_video_render(CPlayer *player);

FFPLAYER_EXPORT void flutter_detach_video_render(CPlayer *player);

#endif //AUDIO_PLAYER_EXAMPLE_FFP_FLUTTER_WINDOWS_H
