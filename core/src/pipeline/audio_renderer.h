#ifndef LYCHEE_PIPELINE_AUDIO_RENDERER_H_
#define LYCHEE_PIPELINE_AUDIO_RENDERER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "media/audio_decoder.h"
#include "media/audio_sink.h"
#include "pipeline/audio_renderer_algorithm.h"
#include "pipeline/media_clock.h"
#include "pipeline/packet_queue.h"

namespace lychee {

// Decodes audio on its own thread and feeds the audio sink. While playing it
// drives the media clock. Rates other than 1 are applied with WSOLA, which
// keeps the pitch.
//
// A device that pulls nothing for a while (it failed to start, or stopped,
// as on a closed laptop lid) is pulled for in real time and its audio
// dropped (PullForStalledDevice()), so playback goes on silently instead of
// waiting for it; the device takes over again when it pulls.
//
// A seek, a pause, a rate change, a track switch or a stop does not cut
// the waveform, which clicks: the device plays on for kFadeDuration,
// fading the audio out, before the sink stops, and the audio after it
// fades in. A loop's passes join without a fade.
//
// For a live input (|live_output_lead| set) the clock runs on its own and
// audio timestamps are deadlines: each sample is played when it is due,
// late ones are dropped and gaps are filled with silence. The renderer
// stores in |live_output_lead| how far ahead of now the device consumes
// audio, for the live timeline.
class AudioRenderer {
 public:
  explicit AudioRenderer(MediaClock* clock,
                         std::atomic<int64_t>* live_output_lead = nullptr);
  ~AudioRenderer();

  bool Initialize(const AudioConfig& config,
                  PacketQueue* queue,
                  std::string* error);
  // Drops audio before |start_time| (encoder priming that an edit list
  // gives negative timestamps), as after a seek; kNoTimestamp drops none.
  void Start(int64_t start_time);
  // The packet queue must be aborted first so the decode thread can exit.
  void Stop();
  // For a renderer being replaced (a track switch): fades out, as a pause
  // does, and stops the decode thread, so the queue can be reset for the
  // next renderer. The sink plays the fade-out on; StopSinkAfterFadeOut()
  // stops it, then SinkStopped() is true and Stop() frees the rest. The
  // queue must be aborted first.
  void Retire();
  bool SinkStopped() const;
  // For a stop: fades out and returns once the device has played it, or
  // after kStopFadeTimeout of real time. Nothing else may call into the
  // renderer meanwhile but the device. Then Stop().
  void FadeOutForStop();

  void Flush(uint64_t generation, int64_t start_time);
  void SetPlaying(bool playing);
  void SetVolume(float volume);
  // Takes effect from the next buffer the sink pulls, after a fade.
  void SetRate(double rate);
  // Call every few milliseconds of |now|: once the device has pulled
  // nothing for kDeviceTimeout while playing, pulls the audio due by |now|
  // in its place. Also retries a sink that failed to start.
  void PullForStalledDevice(int64_t now);
  // Call every few milliseconds of |now|, as PullForStalledDevice(): stops
  // the sink once the device has played the fade-out after a pause.
  void StopSinkAfterFadeOut(int64_t now);

  bool HasEnoughData() const;
  bool IsStarved() const;
  bool Ended() const;
  // Playback ends at |time| (kNoTimestamp: at the end of the stream);
  // audio past it is kept but not played. Cleared by Flush().
  void SetEndTime(int64_t time);
  // Decoded audio lasts |duration| of media time, the buffer is full, or
  // the stream ended. For SettledForTesting().
  bool HasDataFor(int64_t duration) const;
  // The decode thread takes no more packets until audio is played: it
  // waits for buffer space, or it failed. For SettledForTesting().
  bool Blocked() const;
  // The decoder failed for good; decoding has stopped.
  bool HasError() const;
  // Decoded audio waiting to be played, in media time.
  int64_t buffered_duration() const;

  // Every renderer's device pulls nothing while |stalled|, as one that
  // fails to start or stops.
  static void StallDevicesForTesting(bool stalled);
  // The next |count| sink starts, in any renderer, fail.
  static void FailSinkStartsForTesting(int count);

 private:
  // sink_->Start(), unless a test makes it fail. Without the lock: a sink
  // may wait for its own thread, which renders under it.
  bool StartSink();
  // While a start failed (iOS refuses audio in the background), tries
  // again every kSinkStartRetry, so the device plays once it can.
  void RetrySinkStart(int64_t now);
  void DecodeLoop();
  void OnBuffer(std::shared_ptr<AudioBuffer> buffer, uint64_t generation);
  // |from_device|: the sink pulled, not PullForStalledDevice(), which
  // passes no |destination|.
  int Render(float* destination,
             int frames,
             int64_t playout_time,
             bool from_device);
  // Writes the samples due from |playout_time| into |render_bus_|.
  int RenderLiveLocked(int frames, int64_t playout_time);
  // Takes the next kFadeDuration of audio, faded out, for the device to
  // play before the audio playing is cut. Not for live input.
  void FadeOutLocked();
  // Stops the decode thread; the sink plays on.
  void StopDecoding();
  int FadeFrames() const;
  int64_t BufferedDurationLocked() const;
  // Output frames at |rate| left before |end_time_|; INT_MAX without one.
  int FramesBeforeEndLocked(double rate) const;

  MediaClock* const clock_;
  std::atomic<int64_t>* const live_output_lead_;
  PacketQueue* queue_ = nullptr;
  std::unique_ptr<AudioSink> sink_;
  std::unique_ptr<AudioDecoder> decoder_;
  AudioOutputFormat format_;
  std::thread thread_;
  std::atomic<float> volume_{1.0f};
  std::atomic<double> rate_{1.0};

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  // Holds the decoded audio and applies the rate.
  AudioRendererAlgorithm algorithm_;
  // Planar output before interleaving.
  AudioBus render_bus_;
  // Live: decoded audio in deadline order, and the frames of the front
  // buffer already played or dropped.
  std::deque<std::shared_ptr<AudioBuffer>> live_buffers_;
  int live_offset_ = 0;
  int64_t live_frames_ = 0;
  uint64_t generation_ = 0;
  int64_t start_time_ = kNoTimestamp;
  int64_t end_time_ = kNoTimestamp;
  bool end_of_stream_ = false;
  bool failed_ = false;
  // OnBuffer() waits for buffer space.
  bool waiting_ = false;
  bool playing_ = false;
  bool stopped_ = false;
  // When the device last pulled, or playing started.
  int64_t device_pull_time_ = 0;
  // While the device is stalled: when the audio pulled in its place next
  // plays.
  int64_t stalled_playout_time_ = kNoTimestamp;
  // FadeOutLocked()'s audio, and how much of it the device has taken.
  AudioBus fade_out_;
  int fade_out_frames_ = 0;
  int fade_out_offset_ = 0;
  // Frames of the fade-in after a seek or pause played so far.
  int fade_in_frames_ = 0;
  // Started, and not stopped yet: while paused it plays the fade-out.
  bool sink_running_ = false;
  // sink_running_, but the sink's start failed: retried at
  // sink_start_retry_at_.
  bool sink_start_failed_ = false;
  int64_t sink_start_retry_at_ = kNoTimestamp;
  // Paused: when the sink stops. When the device will have played the
  // fade-out, or a timeout for a device that does not take it.
  int64_t stop_sink_at_ = kNoTimestamp;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_AUDIO_RENDERER_H_
