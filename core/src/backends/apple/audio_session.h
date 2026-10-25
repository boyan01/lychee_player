#ifndef LYCHEE_BACKENDS_APPLE_AUDIO_SESSION_H_
#define LYCHEE_BACKENDS_APPLE_AUDIO_SESSION_H_

#include <TargetConditionals.h>

#include <cstdint>
#include <functional>

#if TARGET_OS_IOS

namespace lychee {

// The shared AVAudioSession, for CoreAudioSink on iOS. The app owns the
// session's category; the library only reads it and follows its events.

// Output latency of the current route plus one I/O buffer, in microseconds.
// RemoteIO does not include it in kAudioUnitProperty_Latency, and Bluetooth
// routes add 100-250 ms.
int64_t AudioSessionOutputLatency();

enum class AudioSessionEvent {
  kRouteChanged,         // Headphones, AirPods, a speaker picked by the user.
  kInterruptionEnded,    // After a call or another app's audio; restart.
};

// Calls |callback| on a private serial queue for session events until the
// returned token is passed to RemoveAudioSessionObserver, which waits for a
// running callback to return.
void* AddAudioSessionObserver(std::function<void(AudioSessionEvent)> callback);
void RemoveAudioSessionObserver(void* token);

}  // namespace lychee

#endif  // TARGET_OS_IOS

#endif  // LYCHEE_BACKENDS_APPLE_AUDIO_SESSION_H_
