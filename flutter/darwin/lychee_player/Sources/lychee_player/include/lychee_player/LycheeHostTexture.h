#import <Foundation/Foundation.h>

#if TARGET_OS_OSX
#import <FlutterMacOS/FlutterMacOS.h>
#else
#import <Flutter/Flutter.h>
#endif

#include <lychee/lychee.h>

NS_ASSUME_NONNULL_BEGIN

// A Flutter texture showing the frames of a lychee_player that the host app
// creates and destroys itself, e.g. one fed through
// lychee_player_open_live(). The texture outlives the players shown in it:
// it shows nothing while none is set. Frames are pulled once per display
// refresh, as for the plugin's own players.
@interface LycheeHostTexture : NSObject

// The frame types to create a player for this texture with
// (lychee_config.video_frame_types).
@property(class, readonly) uint32_t frameTypes;

// Registers the texture; main thread.
- (instancetype)initWithRegistry:(NSObject<FlutterTextureRegistry>*)registry;
- (instancetype)init NS_UNAVAILABLE;

@property(readonly) int64_t textureId;

// Shows |player|'s frames, or nothing if NULL. Any thread. Returns once no
// frame of the previous player is being pulled, so call it with NULL before
// lychee_player_destroy().
- (void)setPlayer:(nullable lychee_player*)player;

// Unregisters the texture; main thread, after setPlayer:NULL.
- (void)dispose;

@end

NS_ASSUME_NONNULL_END
