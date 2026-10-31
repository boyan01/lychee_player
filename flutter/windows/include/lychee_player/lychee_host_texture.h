#ifndef FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_
#define FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_

#include <flutter_plugin_registrar.h>
#include <lychee/lychee.h>
#include <stdint.h>

#include "lychee_player_plugin_c_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

// A Flutter texture showing the frames of a lychee_player that the host app
// creates and destroys itself, e.g. one fed through
// lychee_player_open_live(). The texture outlives the players shown in it:
// it shows nothing while none is set. Frames are pulled once per display
// refresh, as for the plugin's own players; D3D11 frames go to Flutter's
// adapter, which the texture gives each player it is set to.
typedef struct LycheeHostTexture LycheeHostTexture;

// The frame types to create a player for this texture with
// (lychee_config.video_frame_types).
FLUTTER_PLUGIN_EXPORT uint32_t LycheeHostTextureFrameTypes(void);

// Registers a texture with |registrar|'s engine; platform thread.
FLUTTER_PLUGIN_EXPORT LycheeHostTexture* LycheeHostTextureCreate(
    FlutterDesktopPluginRegistrarRef registrar);

FLUTTER_PLUGIN_EXPORT int64_t
LycheeHostTextureGetId(LycheeHostTexture* texture);

// Shows |player|'s frames, or nothing if NULL. Any thread. Returns once no
// frame of the previous player is being pulled, so call it with NULL before
// lychee_player_destroy().
FLUTTER_PLUGIN_EXPORT void LycheeHostTextureSetPlayer(
    LycheeHostTexture* texture,
    lychee_player* player);

// Unregisters and frees the texture; platform thread, after
// SetPlayer(NULL).
FLUTTER_PLUGIN_EXPORT void LycheeHostTextureDestroy(LycheeHostTexture* texture);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_LYCHEE_HOST_TEXTURE_H_
