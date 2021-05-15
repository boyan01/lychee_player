//
// Created by boyan on 2021/1/24.
//

#if !defined(FF_PLAYER_FLUTTER_H)
#define FF_PLAYER_FLUTTER_H

#include "ffplayer.h"
#include "ffp_define.h"
#include "media_player.h"

#include "dart/dart_api_dl.h"

using namespace media;

typedef MediaPlayer CPlayer;

FFPLAYER_EXPORT void ffplayer_global_init(void *arg);

FFPLAYER_EXPORT CPlayer *ffp_create_player(PlayerConfiguration *config);

FFPLAYER_EXPORT void ffplayer_free_player(CPlayer *player);

FFPLAYER_EXPORT int ffplayer_open_file(CPlayer *player, const char *filename);

FFPLAYER_EXPORT bool ffplayer_is_paused(CPlayer *player);

FFPLAYER_EXPORT void media_set_play_when_ready(MediaPlayer *player, bool play_when_ready);

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
FFPLAYER_EXPORT void ffp_set_volume(CPlayer *player, double volume);

FFPLAYER_EXPORT double ffp_get_volume(CPlayer *player);

FFPLAYER_EXPORT int ffp_get_state(CPlayer *player);

/**
 * @return -1 if player invalid; -2 if render_context invalid; 0 if none picture rendered.
 */
FFPLAYER_EXPORT double ffp_get_video_aspect_ratio(CPlayer *player);


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

#endif // FF_PLAYER_FLUTTER_H
