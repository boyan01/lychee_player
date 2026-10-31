#ifndef FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_
#define FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>
#include <lychee/lychee.h>

#include "lychee_player_plugin.h"

G_BEGIN_DECLS

// A Flutter texture showing the frames of a lychee_player that the host app
// creates and destroys itself, e.g. one fed through
// lychee_player_open_live(). The texture outlives the players shown in it:
// it shows nothing while none is set. Frames are pulled once per display
// refresh, as for the plugin's own players.
typedef struct _LycheeHostTexture LycheeHostTexture;

// The frame types to create a player for this texture with
// (lychee_config.video_frame_types).
FLUTTER_PLUGIN_EXPORT uint32_t lychee_host_texture_frame_types(void);

// Registers a texture with |registrar|; main thread.
FLUTTER_PLUGIN_EXPORT LycheeHostTexture* lychee_host_texture_new(
    FlTextureRegistrar* registrar);

FLUTTER_PLUGIN_EXPORT int64_t
lychee_host_texture_get_id(LycheeHostTexture* texture);

// Shows |player|'s frames, or nothing if NULL. Any thread. Returns once no
// frame of the previous player is being pulled, so call it with NULL before
// lychee_player_destroy().
FLUTTER_PLUGIN_EXPORT void lychee_host_texture_set_player(
    LycheeHostTexture* texture,
    lychee_player* player);

// Unregisters and frees the texture; main thread, after set_player(NULL).
FLUTTER_PLUGIN_EXPORT void lychee_host_texture_free(LycheeHostTexture* texture);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_
