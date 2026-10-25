#include "backends/apple/audio_session.h"

#if TARGET_OS_IOS

#import <AVFAudio/AVFAudio.h>

#include "base/time.h"

namespace lychee {

int64_t AudioSessionOutputLatency() {
  AVAudioSession* session = AVAudioSession.sharedInstance;
  return static_cast<int64_t>(
      (session.outputLatency + session.IOBufferDuration) * kMicrosPerSecond);
}

void* AddAudioSessionObserver(std::function<void(AudioSessionEvent)> callback) {
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  auto shared = std::make_shared<std::function<void(AudioSessionEvent)>>(
      std::move(callback));
  // A private serial queue, so removal can wait for a running callback.
  NSOperationQueue* queue = [[NSOperationQueue alloc] init];
  queue.maxConcurrentOperationCount = 1;
  id route = [center
      addObserverForName:AVAudioSessionRouteChangeNotification
                  object:nil
                   queue:queue
              usingBlock:^(NSNotification*) {
                (*shared)(AudioSessionEvent::kRouteChanged);
              }];
  id interruption = [center
      addObserverForName:AVAudioSessionInterruptionNotification
                  object:nil
                   queue:queue
              usingBlock:^(NSNotification* notification) {
                NSNumber* type =
                    notification.userInfo[AVAudioSessionInterruptionTypeKey];
                if (type.unsignedIntegerValue ==
                    AVAudioSessionInterruptionTypeEnded) {
                  (*shared)(AudioSessionEvent::kInterruptionEnded);
                }
              }];
  return (__bridge_retained void*)@[ queue, route, interruption ];
}

void RemoveAudioSessionObserver(void* token) {
  NSArray* objects = (__bridge_transfer NSArray*)token;
  NSOperationQueue* queue = objects[0];
  for (NSUInteger i = 1; i < objects.count; ++i) {
    [NSNotificationCenter.defaultCenter removeObserver:objects[i]];
  }
  // A notification delivered before removal may still be queued or running.
  [queue waitUntilAllOperationsAreFinished];
}

}  // namespace lychee

#endif  // TARGET_OS_IOS
