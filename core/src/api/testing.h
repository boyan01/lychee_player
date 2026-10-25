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
// threads: nothing is loading, seeking or switching, and decoded data
// reaches that far. Wait for it before each lychee_testing_set_time().
LYCHEE_EXPORT int32_t lychee_testing_player_settled(lychee_player* player,
                                                    int64_t until_us);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYCHEE_API_TESTING_H_
