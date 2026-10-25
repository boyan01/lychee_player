// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/audio_renderer_algorithm.h and
// media/filters/wsola_internals.h, with interleaved lychee::AudioBuffer
// input, a minimal planar AudioBus, and microsecond timestamps. Left out:
// the resampler mode (pitch is always preserved), channel masks, latency
// hints and queue capacity (AudioRenderer limits decoding itself), bitstream
// formats, and the SIMD dot products.

#ifndef LYCHEE_PIPELINE_AUDIO_RENDERER_ALGORITHM_H_
#define LYCHEE_PIPELINE_AUDIO_RENDERER_ALGORITHM_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "base/time.h"
#include "media/audio_buffer.h"

namespace lychee {

// Planar float audio: |channels| vectors of |frames| samples.
class AudioBus {
 public:
  AudioBus() = default;
  AudioBus(int channels, int frames);

  int channels() const { return static_cast<int>(data_.size()); }
  int frames() const { return frames_; }
  float* channel(int index) { return data_[index].data(); }
  const float* channel(int index) const { return data_[index].data(); }

  // Keeps the channel count; contents become unspecified.
  void Resize(int frames);
  void Zero();
  void ZeroFrames(int frames);
  void CopyPartialFramesTo(int source_offset,
                           int frames,
                           int dest_offset,
                           AudioBus* dest) const;

 private:
  std::vector<std::vector<float>> data_;
  int frames_ = 0;
};

// Speeds audio up or slows it down without changing its pitch. The owner
// enqueues decoded buffers and pulls output through FillBuffer().
//
// Uses the Waveform Similarity Overlap and Add (WSOLA) algorithm: output is
// built from overlapping windows of input; each next window is the one
// within a search region (around where the playback rate says the input
// should be) that best continues the output so far. Local properties of the
// audio, hence pitch and harmonics, are preserved.
//
// Not thread-safe.
class AudioRendererAlgorithm {
 public:
  enum class FillBufferMode { kPassthrough, kWSOLA };

  AudioRendererAlgorithm();
  ~AudioRendererAlgorithm();

  void Initialize(int channels, int sample_rate);

  // Writes up to |requested_frames| frames into |dest| at |dest_offset|,
  // consuming input in proportion to |playback_rate|. Returns the number of
  // frames written. Rates close to 1 copy the input unchanged.
  int FillBuffer(AudioBus* dest,
                 int dest_offset,
                 int requested_frames,
                 double playback_rate);

  // Drops all queued input and WSOLA state.
  void FlushBuffers();

  // |buffer| must have the channel count and sample rate passed to
  // Initialize().
  void EnqueueBuffer(std::shared_ptr<const AudioBuffer> buffer);

  // Input frames not yet consumed.
  int BufferedFrames() const { return queue_.frames(); }

  // Media time of the input the next output frame comes from, or
  // kNoTimestamp if nothing was ever enqueued. Approximate under WSOLA, which
  // repeats or skips input.
  int64_t NextOutputTimestamp() const;

  int sample_rate() const { return sample_rate_; }
  FillBufferMode last_mode() const { return last_mode_; }

 private:
  // A queue of interleaved buffers, read as one planar stream.
  class BufferQueue {
   public:
    void Append(std::shared_ptr<const AudioBuffer> buffer);
    void Clear();
    int frames() const { return frames_; }
    // Copies |count| frames starting |source_offset| frames into the queue
    // to |dest| at |dest_offset| without consuming them.
    void PeekFrames(int count,
                    int source_offset,
                    int dest_offset,
                    AudioBus* dest) const;
    // Copies and consumes.
    int ReadFrames(int count, int dest_offset, AudioBus* dest);
    void SeekFrames(int count);
    // Media time of the first queued frame, or kNoTimestamp if unknown.
    int64_t FrontTimestamp() const;
    // Media time just past the last frame consumed.
    int64_t consumed_end() const { return consumed_end_; }

   private:
    std::deque<std::shared_ptr<const AudioBuffer>> buffers_;
    // Frames of buffers_.front() already consumed.
    int front_offset_ = 0;
    int frames_ = 0;
    int64_t consumed_end_ = 0;
    bool has_timestamp_ = false;
  };

  FillBufferMode ChooseBufferMode(double playback_rate) const;
  // Drops WSOLA state when the mode changes, so it does not leak stale
  // audio later.
  void SetFillBufferMode(FillBufferMode mode);
  int RunWsolaAndFill(AudioBus* dest,
                      int dest_offset,
                      int requested_frames,
                      double playback_rate);
  // Within |search_block_|, finds the block most similar to |target_block_|
  // and writes it to |optimal_block_|. Assumes there is enough input.
  void GetOptimalBlock();
  // Moves up to |requested_frames| completed frames from |wsola_output_|.
  int WriteCompletedFramesTo(int requested_frames,
                             int dest_offset,
                             AudioBus* dest);
  // Copies input from |read_offset_frames| on, which may be negative: input
  // before the start counts as silence.
  void PeekAudioWithZeroPrepend(int read_offset_frames, AudioBus* dest);
  // Overlap-and-adds one block to |wsola_output_| if there is enough input.
  bool RunOneWsolaIteration(double playback_rate);
  // Drops input that no later iteration can use.
  void RemoveOldInputFrames(double playback_rate);
  void UpdateOutputTime(double playback_rate, double time_change);
  bool TargetIsWithinSearchRegion() const;
  bool CanPerformWsola() const;

  int channels_ = 0;
  int sample_rate_ = 0;
  BufferQueue queue_;

  // Time of the generated output, in frames, relative to the front of the
  // queue. Update only through UpdateOutputTime(), which keeps
  // |search_block_index_| in step.
  double output_time_ = 0;
  // Offset of the center frame of |search_block_| from its first frame.
  int search_block_center_offset_ = 0;
  // First frame of the search block, relative to the front of the queue.
  int search_block_index_ = 0;
  // Candidate blocks searched for the one most similar to the target.
  int num_candidate_blocks_ = 0;
  // First frame of the target block, relative to the front of the queue.
  int target_block_index_ = 0;
  int ola_window_size_ = 0;
  // 50% overlap.
  int ola_hop_size_ = 0;
  // Frames of |wsola_output_| that are complete and can be output; also
  // where the next window is overlap-added.
  int num_complete_frames_ = 0;

  // Output that was generated but not yet requested, and the incomplete
  // half window after it.
  AudioBus wsola_output_;
  std::vector<float> ola_window_;
  // Blends |optimal_block_| towards |target_block_|.
  std::vector<float> transition_window_;
  AudioBus optimal_block_;
  AudioBus search_block_;
  AudioBus target_block_;

  FillBufferMode last_mode_ = FillBufferMode::kPassthrough;
  double last_playback_rate_ = 1.0;
};

namespace wsola {

// [first, second], inclusive. Chromium uses size_t here; ints keep an
// interval starting before 0 meaningful.
using Interval = std::pair<int, int>;

// Dot products of the channels of |a| and |b| from the given offsets.
// |dot_product| receives one value per channel.
void MultiChannelDotProduct(const AudioBus& a,
                            int frame_offset_a,
                            const AudioBus& b,
                            int frame_offset_b,
                            int num_frames,
                            float* dot_product);

// Energies of every |frames_per_block| window of |input|, channels
// interleaved: input.frames() - frames_per_block + 1 blocks.
void MultiChannelMovingBlockEnergies(const AudioBus& input,
                                     int frames_per_block,
                                     float* energy);

// Fits f(x) = a x^2 + b x + c through f(-1) = y[0], f(0) = y[1], f(1) = y[2]
// and returns its extremum, assuming y[0] <= y[1] >= y[2].
void QuadraticInterpolation(const float* y_values,
                            float* extremum,
                            float* extremum_value);

// Searches every |decimation|-th candidate block, refining local maxima by
// quadratic interpolation.
int DecimatedSearch(int decimation,
                    Interval exclude_interval,
                    const AudioBus& target_block,
                    const AudioBus& search_segment,
                    const float* energy_target_block,
                    const float* energy_candidate_blocks);

// Searches candidates [low_limit, high_limit] of |search_block| for the one
// most similar to |target_block|.
int FullSearch(int low_limit,
               int high_limit,
               Interval exclude_interval,
               const AudioBus& target_block,
               const AudioBus& search_block,
               const float* energy_target_block,
               const float* energy_candidate_blocks);

// Index within |search_block| of the block most similar to |target_block|,
// outside |exclude_interval|.
int OptimalIndex(const AudioBus& search_block,
                 const AudioBus& target_block,
                 Interval exclude_interval);

// The first window.size() samples of a window.size() + 1 Hann window: perfect
// reconstruction for overlap-and-add.
void GetPeriodicHanningWindow(std::vector<float>* window);

}  // namespace wsola

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_AUDIO_RENDERER_ALGORITHM_H_
