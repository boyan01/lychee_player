//
// Created by boyan on 2021/1/24.
//

#ifdef _FLUTTER
#ifndef FF_PLAYER_FLUTTER_H
#define FF_PLAYER_FLUTTER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _FLUTTER_WINDOWS
#include <flutter/plugin_registrar_windows.h>
#endif

#include "ffplayer.h"
#include "include/third_party/dart/dart_api_dl.h"

FFPLAYER_EXPORT void ffplayer_global_init(void *arg);

FFPLAYER_EXPORT CPlayer *ffp_create_player(FFPlayerConfiguration *config);

FFPLAYER_EXPORT void ffplayer_free_player(CPlayer *player);

FFPLAYER_EXPORT void ffp_attach_video_render(CPlayer *player, FFP_VideoRenderCallback *render_callback);

FFPLAYER_EXPORT int ffplayer_open_file(CPlayer *player, const char *filename);

FFPLAYER_EXPORT bool ffplayer_is_paused(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_toggle_pause(CPlayer *player);

FFPLAYER_EXPORT bool ffplayer_is_mute(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_set_mute(CPlayer *player, bool mute);

FFPLAYER_EXPORT int ffplayer_get_current_chapter(CPlayer *player);

FFPLAYER_EXPORT int ffplayer_get_chapter_count(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_seek_to_chapter(CPlayer *player, int chapter);

FFPLAYER_EXPORT double ffplayer_get_current_position(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_seek_to_position(CPlayer *player, double position);

FFPLAYER_EXPORT double ffplayer_get_duration(CPlayer *player);

FFPLAYER_EXPORT const char *ffp_get_file_name(CPlayer *player);

FFPLAYER_EXPORT const char *ffp_get_metadata_dict(CPlayer *player, const char *key);

/**
 * @param volume from 0 to 100.
 */
FFPLAYER_EXPORT void ffp_set_volume(CPlayer *player, int volume);

FFPLAYER_EXPORT int ffp_get_volume(CPlayer *player);

FFPLAYER_EXPORT int ffp_get_state(CPlayer *player);

FFPLAYER_EXPORT void
ffp_set_message_callback(CPlayer *player, void (*callback)(CPlayer *, int32_t, int64_t, int64_t));

FFPLAYER_EXPORT void ffp_refresh_texture(CPlayer *player);

/**
 * @return -1 if player invalid; -2 if render_context invalid; 0 if none picture rendered.
 */
FFPLAYER_EXPORT double ffp_get_video_aspect_ratio(CPlayer *player);

#ifdef _FLUTTER

/**
 * Set Message callback for dart. Post message to dart isolate by @param send_port.
 */
FFPLAYER_EXPORT void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port);

/**
 * Attach flutter render texture.
 *
 * @return the texture id which attached.
 */
FFPLAYER_EXPORT int64_t ffp_attach_video_render_flutter(CPlayer *player);

FFPLAYER_EXPORT void ffp_detach_video_render_flutter(CPlayer *player);

#endif

#ifdef _FLUTTER_WINDOWS
FFPLAYER_EXPORT void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar);
#endif

#ifdef __cplusplus
}
#endif

#endif //FF_PLAYER_FLUTTER_H

#endif // _FLUTTER