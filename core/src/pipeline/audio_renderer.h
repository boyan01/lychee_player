#ifndef LYCHEE_PIPELINE_AUDIO_RENDERER_H_
#define LYCHEE_PIPELINE_AUDIO_RENDERER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
class AudioRenderer {
 public:
  explicit AudioRenderer(MediaClock* clock);
  ~AudioRenderer();

  bool Initialize(const AudioConfig& config,
                  PacketQueue* queue,
                  std::string* error);
  void Start();
  // The packet queue must be aborted first so the decode thread can exit.
  void Stop();

  void Flush(uint64_t generation, int64_t start_time);
  void SetPlaying(bool playing);
  void SetVolume(float volume);
  // Takes effect from the next buffer the sink pulls.
  void SetRate(double rate);

  bool HasEnoughData() const;
  bool IsStarved() const;
  bool Ended() const;
  // The decoder failed for good; decoding has stopped.
  bool HasError() const;
  // Decoded audio waiting to be played, in media time.
  int64_t buffered_duration() const;

 private:
  void DecodeLoop();
  void OnBuffer(std::shared_ptr<AudioBuffer> buffer, uint64_t generation);
  int Render(float* destination, int frames, int64_t playout_time);
  int64_t BufferedDurationLocked() const;

  MediaClock* const clock_;
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
  // Render thread only: planar output before interleaving.
  AudioBus render_bus_;
  uint64_t generation_ = 0;
  int64_t start_time_ = kNoTimestamp;
  bool end_of_stream_ = false;
  bool failed_ = false;
  bool playing_ = false;
  bool stopped_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_AUDIO_RENDERER_H_
