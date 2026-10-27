// Test hooks for examples/cli; not part of the C ABI in lychee/lychee.h.
// Virtual time lets the CLI play media as fast as it decodes, with results
// that do not depend on machine load: time stands still until the caller
// moves it, and audio goes to a silent device that plays exactly the audio
// due in the time moved over.

#ifndef LYCHEE_API_TESTING_H_
#define LYCHEE_API_TESTING_H_

#include "lychee/lychee.h"

#ifdef __cplusplus
extern "C" {
#endif

// Switches lychee_now_us() to virtual time, starting at the current time.
// Call before creating a player.
LYCHEE_EXPORT void lychee_testing_enable_virtual_time(void);
// Moves virtual time forward to |now_us|, rendering the audio due by then.
LYCHEE_EXPORT void lychee_testing_set_time(int64_t now_us);
// Non-zero if |player| can play on to |until_us| without waiting for its
// threads: nothing is loading, seeking or switching, decoded data
// reaches that far, and the event callback has seen every event so far.
// Wait for it before each lychee_testing_set_time(). For a live input,
// instead of the data: every packet pushed so far has been decoded.
LYCHEE_EXPORT int32_t lychee_testing_player_settled(lychee_player* player,
                                                    int64_t until_us);
// Makes the video decoder fail at its next packet as a hardware decoder
// does after a GPU reset, so the player rebuilds it.
LYCHEE_EXPORT void lychee_testing_lose_video_decoder(lychee_player* player);
// Makes the video decoder hang at its next packet, as a driver that never
// returns, until the player gives up on it.
LYCHEE_EXPORT void lychee_testing_hang_video_decoder(lychee_player* player);
// Non-zero: every player's audio device stops pulling audio, as one that
// fails to start or stops; zero: it pulls again.
LYCHEE_EXPORT void lychee_testing_stall_audio_devices(int32_t stalled);

// A file replayed as a live input: its default video and audio streams,
// in file order, as lychee_player_push_live_packet() takes them (Annex B
// video, parameter sets before every keyframe).
typedef struct lychee_testing_live_source lychee_testing_live_source;
// NULL if |url| cannot be opened.
LYCHEE_EXPORT lychee_testing_live_source* lychee_testing_live_source_open(
    const char* url);
// The file's |type| track as a live track. Returns -1 if there is none or
// its codec is not one live input takes.
LYCHEE_EXPORT int32_t lychee_testing_live_source_track(
    lychee_testing_live_source* source,
    int32_t type,
    lychee_live_track* track);
// The next packet of a track that lychee_testing_live_source_track()
// returned; |packet->data| stays valid until the next call. |send_time_us|
// is when a sender would send it: its decode timestamp, on the same clock
// as |packet->pts_us|. Returns -1 at the end.
LYCHEE_EXPORT int32_t lychee_testing_live_source_read(
    lychee_testing_live_source* source,
    lychee_live_packet* packet,
    int64_t* send_time_us);
LYCHEE_EXPORT void lychee_testing_live_source_close(
    lychee_testing_live_source* source);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYCHEE_API_TESTING_H_
