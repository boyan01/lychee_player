// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/audio_renderer_algorithm.cc and
// media/filters/wsola_internals.cc. See the header for what was left out.

#include "pipeline/audio_renderer_algorithm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <numeric>

namespace lychee {

// Waveform Similarity Overlap-and-add (WSOLA).
//
// One WSOLA iteration
//
// 1) Extract |target_block_| as input frames at indices
//    [|target_block_index_|, |target_block_index_| + |ola_window_size_|).
//    Note that |target_block_| is the "natural" continuation of the output.
//
// 2) Extract |search_block_| as input frames at indices
//    [|search_block_index_|,
//     |search_block_index_| + |num_candidate_blocks_| + |ola_window_size_|).
//
// 3) Find a block within the |search_block_| that is most similar
//    to |target_block_|. Let |optimal_index| be the index of such block and
//    write it to |optimal_block_|.
//
// 4) Update:
//    |optimal_block_| = |transition_window_| * |target_block_| +
//    (1 - |transition_window_|) * |optimal_block_|.
//
// 5) Overlap-and-add |optimal_block_| to the |wsola_output_|.
//
// 6) Update:
//    |target_block_| = |optimal_index| + |ola_window_size_| / 2.
//    |output_index_| = |output_index_| + |ola_window_size_| / 2,
//    |search_block_center_index| = |output_index_| * |playback_rate|, and
//    |search_block_index_| = |search_block_center_index| -
//        |search_block_center_offset_|.

namespace {

// Overlap-and-add window size.
constexpr int64_t kOlaWindowSizeUs = 20 * kMicrosPerMilli;
// The search interval is [-delta, delta] around
// |output_index_| * |playback_rate|, so it spans 2 * delta.
constexpr int64_t kWsolaSearchIntervalUs = 30 * kMicrosPerMilli;
// An interval around the last optimal block excluded from the search, to
// reduce a buzzy sound. Chromium derived 160 heuristically.
constexpr int kExcludeIntervalLengthFrames = 160;

int TimeToFrames(int64_t time, int sample_rate) {
  return static_cast<int>(time * sample_rate / kMicrosPerSecond);
}

}  // namespace

AudioBus::AudioBus(int channels, int frames)
    : data_(channels, std::vector<float>(frames)), frames_(frames) {}

void AudioBus::Resize(int frames) {
  for (auto& channel : data_) {
    channel.resize(frames);
  }
  frames_ = frames;
}

void AudioBus::Zero() {
  ZeroFrames(frames_);
}

void AudioBus::ZeroFrames(int frames) {
  for (auto& channel : data_) {
    std::fill(channel.begin(), channel.begin() + frames, 0.0f);
  }
}

void AudioBus::CopyPartialFramesTo(int source_offset,
                                   int frames,
                                   int dest_offset,
                                   AudioBus* dest) const {
  for (int k = 0; k < channels(); ++k) {
    memcpy(dest->channel(k) + dest_offset, channel(k) + source_offset,
           sizeof(float) * frames);
  }
}

void AudioRendererAlgorithm::BufferQueue::Append(
    std::shared_ptr<const AudioBuffer> buffer) {
  if (buffer->frames() == 0) {
    return;
  }
  if (!has_timestamp_ && buffer->pts != kNoTimestamp) {
    consumed_end_ = buffer->pts;
    has_timestamp_ = true;
  }
  frames_ += buffer->frames();
  buffers_.push_back(std::move(buffer));
}

void AudioRendererAlgorithm::BufferQueue::Clear() {
  buffers_.clear();
  front_offset_ = 0;
  frames_ = 0;
  consumed_end_ = 0;
  has_timestamp_ = false;
}

void AudioRendererAlgorithm::BufferQueue::PeekFrames(int count,
                                                     int source_offset,
                                                     int dest_offset,
                                                     AudioBus* dest) const {
  int offset = front_offset_ + source_offset;
  for (const auto& buffer : buffers_) {
    if (count == 0) {
      break;
    }
    const int frames = buffer->frames();
    if (offset >= frames) {
      offset -= frames;
      continue;
    }
    const int n = std::min(frames - offset, count);
    const int channels = buffer->channels;
    const float* source =
        buffer->samples.data() + static_cast<ptrdiff_t>(offset) * channels;
    for (int k = 0; k < dest->channels(); ++k) {
      float* out = dest->channel(k) + dest_offset;
      for (int i = 0; i < n; ++i) {
        out[i] = source[i * channels + k];
      }
    }
    dest_offset += n;
    count -= n;
    offset = 0;
  }
}

int AudioRendererAlgorithm::BufferQueue::ReadFrames(int count,
                                                    int dest_offset,
                                                    AudioBus* dest) {
  count = std::min(count, frames_);
  PeekFrames(count, 0, dest_offset, dest);
  SeekFrames(count);
  return count;
}

void AudioRendererAlgorithm::BufferQueue::SeekFrames(int count) {
  count = std::min(count, frames_);
  frames_ -= count;
  while (count > 0) {
    const AudioBuffer& front = *buffers_.front();
    const int n = std::min(front.frames() - front_offset_, count);
    front_offset_ += n;
    count -= n;
    if (front.pts != kNoTimestamp) {
      consumed_end_ = front.pts + static_cast<int64_t>(front_offset_) *
                                      kMicrosPerSecond / front.sample_rate;
    } else {
      consumed_end_ += static_cast<int64_t>(n) * kMicrosPerSecond /
                       front.sample_rate;
    }
    if (front_offset_ == front.frames()) {
      buffers_.pop_front();
      front_offset_ = 0;
    }
  }
}

void AudioRendererAlgorithm::BufferQueue::EndAt(int64_t time,
                                                int channels,
                                                int sample_rate) {
  while (!buffers_.empty() && buffers_.back()->pts != kNoTimestamp) {
    const AudioBuffer& back = *buffers_.back();
    const int consumed = buffers_.size() == 1 ? front_offset_ : 0;
    const int keep = static_cast<int>(std::clamp<int64_t>(
        std::llround(static_cast<double>(time - back.pts) * back.sample_rate /
                     kMicrosPerSecond),
        consumed, back.frames()));
    if (keep == back.frames()) {
      break;
    }
    frames_ -= back.frames() - keep;
    if (keep == consumed) {
      buffers_.pop_back();
      front_offset_ = buffers_.empty() ? 0 : front_offset_;
      continue;
    }
    auto cut = std::make_shared<AudioBuffer>(back);
    cut->samples.resize(static_cast<size_t>(keep) * cut->channels);
    buffers_.back() = std::move(cut);
    break;
  }
  if (!has_timestamp_) {
    return;
  }
  int64_t end = consumed_end_;
  if (!buffers_.empty()) {
    const AudioBuffer& back = *buffers_.back();
    if (back.pts == kNoTimestamp) {
      return;
    }
    end = back.pts + back.duration();
  }
  const int64_t gap = std::llround(static_cast<double>(time - end) *
                                   sample_rate / kMicrosPerSecond);
  if (gap > 0) {
    auto silence = std::make_shared<AudioBuffer>();
    silence->pts = end;
    silence->sample_rate = sample_rate;
    silence->channels = channels;
    silence->samples.assign(static_cast<size_t>(gap) * channels, 0.0f);
    Append(std::move(silence));
  }
}

int64_t AudioRendererAlgorithm::BufferQueue::FrontTimestamp() const {
  if (!has_timestamp_) {
    return kNoTimestamp;
  }
  if (!buffers_.empty() && buffers_.front()->pts != kNoTimestamp) {
    const AudioBuffer& front = *buffers_.front();
    return front.pts + static_cast<int64_t>(front_offset_) *
                           kMicrosPerSecond / front.sample_rate;
  }
  return consumed_end_;
}

AudioRendererAlgorithm::AudioRendererAlgorithm() = default;
AudioRendererAlgorithm::~AudioRendererAlgorithm() = default;

void AudioRendererAlgorithm::Initialize(int channels, int sample_rate) {
  channels_ = channels;
  sample_rate_ = sample_rate;
  num_candidate_blocks_ = TimeToFrames(kWsolaSearchIntervalUs, sample_rate);
  ola_window_size_ = TimeToFrames(kOlaWindowSizeUs, sample_rate);

  // Make sure window size is an even number.
  ola_window_size_ += ola_window_size_ & 1;
  ola_hop_size_ = ola_window_size_ / 2;

  // |num_candidate_blocks_| / 2 is the offset of the center of the search
  // block to the center of the first (left most) candidate block. The offset
  // of the center of a candidate block to its left most point is
  // |ola_window_size_| / 2 - 1. Note that |ola_window_size_| is even and in
  // our convention the center belongs to the left half, so we need to subtract
  // one frame to get the correct offset.
  //
  //                             Search Block
  //              <------------------------------------------->
  //
  //   |ola_window_size_| / 2 - 1
  //              <----
  //
  //             |num_candidate_blocks_| / 2
  //                   <----------------
  //                                 center
  //              X----X----------------X---------------X-----X
  //              <---------->                     <---------->
  //                Candidate      ...               Candidate
  //                   1,          ...         |num_candidate_blocks_|
  search_block_center_offset_ =
      num_candidate_blocks_ / 2 + (ola_window_size_ / 2 - 1);
}

int AudioRendererAlgorithm::RunWsolaAndFill(AudioBus* dest,
                                            int dest_offset,
                                            int requested_frames,
                                            double playback_rate) {
  // Allocate structures on first non-1.0 playback rate; these can eat a fair
  // chunk of memory. ~56kB for stereo 48kHz, up to ~765kB for 7.1 192kHz.
  if (ola_window_.empty()) {
    ola_window_.resize(ola_window_size_);
    wsola::GetPeriodicHanningWindow(&ola_window_);

    transition_window_.resize(static_cast<size_t>(ola_window_size_) * 2);
    wsola::GetPeriodicHanningWindow(&transition_window_);

    // Initialize for overlap-and-add of the first block.
    wsola_output_ = AudioBus(channels_, ola_window_size_ + ola_hop_size_);

    // Auxiliary containers.
    optimal_block_ = AudioBus(channels_, ola_window_size_);
    search_block_ =
        AudioBus(channels_, num_candidate_blocks_ + (ola_window_size_ - 1));
    target_block_ = AudioBus(channels_, ola_window_size_);
  }

  int rendered_frames = 0;
  do {
    rendered_frames +=
        WriteCompletedFramesTo(requested_frames - rendered_frames,
                               dest_offset + rendered_frames, dest);
  } while (rendered_frames < requested_frames &&
           RunOneWsolaIteration(playback_rate));
  return rendered_frames;
}

int AudioRendererAlgorithm::FillBuffer(AudioBus* dest,
                                       int dest_offset,
                                       int requested_frames,
                                       double playback_rate) {
  if (playback_rate <= 0) {
    return 0;
  }

  const FillBufferMode fill_buffer_mode = ChooseBufferMode(playback_rate);
  SetFillBufferMode(fill_buffer_mode);
  last_playback_rate_ = playback_rate;

  switch (fill_buffer_mode) {
    case FillBufferMode::kPassthrough:
      // Optimize the most common |playback_rate| ~= 1 case to use a single
      // copy instead of copying frame by frame.
      return queue_.ReadFrames(requested_frames, dest_offset, dest);
    case FillBufferMode::kWSOLA:
      return RunWsolaAndFill(dest, dest_offset, requested_frames,
                             playback_rate);
  }
  return 0;
}

AudioRendererAlgorithm::FillBufferMode AudioRendererAlgorithm::ChooseBufferMode(
    double playback_rate) const {
  const int slower_step =
      static_cast<int>(std::ceil(ola_window_size_ * playback_rate));
  const int faster_step =
      static_cast<int>(std::ceil(ola_window_size_ / playback_rate));

  // Rates within one frame per window of 1.0 are played unchanged.
  if (ola_window_size_ <= faster_step && slower_step >= ola_window_size_) {
    return FillBufferMode::kPassthrough;
  }
  return FillBufferMode::kWSOLA;
}

void AudioRendererAlgorithm::SetFillBufferMode(FillBufferMode mode) {
  if (last_mode_ == mode) {
    return;
  }
  // Clear any state from other fill modes so that we don't produce outdated
  // audio later.
  if (last_mode_ == FillBufferMode::kWSOLA) {
    output_time_ = 0.0;
    search_block_index_ = 0;
    target_block_index_ = 0;
    if (wsola_output_.frames()) {
      wsola_output_.Zero();
    }
    num_complete_frames_ = 0;
  }
  last_mode_ = mode;
}

void AudioRendererAlgorithm::FlushBuffers() {
  // Clear the queue of decoded packets (releasing the buffers).
  queue_.Clear();
  output_time_ = 0.0;
  search_block_index_ = 0;
  target_block_index_ = 0;
  if (wsola_output_.frames()) {
    wsola_output_.Zero();
  }
  num_complete_frames_ = 0;
}

void AudioRendererAlgorithm::EnqueueBuffer(
    std::shared_ptr<const AudioBuffer> buffer) {
  queue_.Append(std::move(buffer));
}

int64_t AudioRendererAlgorithm::NextOutputTimestamp() const {
  int64_t front = queue_.FrontTimestamp();
  if (front == kNoTimestamp || last_mode_ != FillBufferMode::kWSOLA) {
    return front;
  }
  // Not in Chromium, which derives this from DelayInFrames(): the search
  // center sits at |output_time_| * rate input frames, and the completed but
  // unread output comes before it.
  const double input_frames =
      (output_time_ - num_complete_frames_) * last_playback_rate_;
  return front + static_cast<int64_t>(input_frames * kMicrosPerSecond /
                                      sample_rate_);
}

bool AudioRendererAlgorithm::CanPerformWsola() const {
  const int search_block_size = num_candidate_blocks_ + (ola_window_size_ - 1);
  const int frames = queue_.frames();
  return target_block_index_ + ola_window_size_ <= frames &&
         search_block_index_ + search_block_size <= frames;
}

bool AudioRendererAlgorithm::RunOneWsolaIteration(double playback_rate) {
  if (!CanPerformWsola()) {
    return false;
  }

  GetOptimalBlock();

  // Overlap-and-add.
  for (int k = 0; k < channels_; ++k) {
    const float* ch_opt_frame = optimal_block_.channel(k);
    float* ch_output = wsola_output_.channel(k) + num_complete_frames_;
    for (int n = 0; n < ola_hop_size_; ++n) {
      ch_output[n] = ch_output[n] * ola_window_[ola_hop_size_ + n] +
                     ch_opt_frame[n] * ola_window_[n];
    }

    // Copy the second half to the output.
    memcpy(&ch_output[ola_hop_size_], &ch_opt_frame[ola_hop_size_],
           sizeof(*ch_opt_frame) * ola_hop_size_);
  }

  num_complete_frames_ += ola_hop_size_;
  UpdateOutputTime(playback_rate, ola_hop_size_);
  RemoveOldInputFrames(playback_rate);
  return true;
}

void AudioRendererAlgorithm::UpdateOutputTime(double playback_rate,
                                              double time_change) {
  output_time_ += time_change;
  // Center of the search region, in frames.
  const int search_block_center_index =
      static_cast<int>(std::lround(output_time_ * playback_rate));
  search_block_index_ = search_block_center_index - search_block_center_offset_;
}

void AudioRendererAlgorithm::RemoveOldInputFrames(double playback_rate) {
  const int earliest_used_index =
      std::min(target_block_index_, search_block_index_);
  if (earliest_used_index <= 0) {
    return;  // Nothing to remove.
  }

  // Remove frames from input and adjust indices accordingly.
  queue_.SeekFrames(earliest_used_index);
  target_block_index_ -= earliest_used_index;

  // Adjust output index.
  const double output_time_change =
      static_cast<double>(earliest_used_index) / playback_rate;
  UpdateOutputTime(playback_rate, -output_time_change);
}

int AudioRendererAlgorithm::WriteCompletedFramesTo(int requested_frames,
                                                   int dest_offset,
                                                   AudioBus* dest) {
  const int rendered_frames = std::min(num_complete_frames_, requested_frames);
  if (rendered_frames == 0) {
    return 0;  // There is nothing to read from |wsola_output_|, return.
  }

  wsola_output_.CopyPartialFramesTo(0, rendered_frames, dest_offset, dest);

  // Remove the frames which are read.
  const int frames_to_move = wsola_output_.frames() - rendered_frames;
  for (int k = 0; k < channels_; ++k) {
    float* ch = wsola_output_.channel(k);
    memmove(ch, &ch[rendered_frames], sizeof(*ch) * frames_to_move);
  }
  num_complete_frames_ -= rendered_frames;
  return rendered_frames;
}

bool AudioRendererAlgorithm::TargetIsWithinSearchRegion() const {
  const int search_block_size = num_candidate_blocks_ + (ola_window_size_ - 1);
  return target_block_index_ >= search_block_index_ &&
         target_block_index_ + ola_window_size_ <=
             search_block_index_ + search_block_size;
}

void AudioRendererAlgorithm::GetOptimalBlock() {
  int optimal_index = 0;

  if (TargetIsWithinSearchRegion()) {
    optimal_index = target_block_index_;
    PeekAudioWithZeroPrepend(optimal_index, &optimal_block_);
  } else {
    PeekAudioWithZeroPrepend(target_block_index_, &target_block_);
    PeekAudioWithZeroPrepend(search_block_index_, &search_block_);
    const int last_optimal =
        target_block_index_ - ola_hop_size_ - search_block_index_;
    const wsola::Interval exclude_interval =
        std::make_pair(last_optimal - kExcludeIntervalLengthFrames / 2,
                       last_optimal + kExcludeIntervalLengthFrames / 2);

    // |optimal_index| is in frames and it is relative to the beginning of the
    // |search_block_|.
    optimal_index =
        wsola::OptimalIndex(search_block_, target_block_, exclude_interval);

    // Translate |index| w.r.t. the beginning of the queue and extract the
    // optimal block.
    optimal_index += search_block_index_;
    PeekAudioWithZeroPrepend(optimal_index, &optimal_block_);

    // Make a transition from target block to the optimal block if different.
    // Target block has the best continuation to the current output.
    // Optimal block is the most similar block to the target, however, it might
    // introduce some discontinuity when over-lap-added. Therefore, we combine
    // them for a smoother transition. The length of transition window is twice
    // as that of the optimal-block which makes it like a weighting function
    // where target-block has higher weight close to zero (weight of 1 at index
    // 0) and lower weight close the end.
    for (int k = 0; k < channels_; ++k) {
      float* ch_opt = optimal_block_.channel(k);
      const float* ch_target = target_block_.channel(k);
      for (int n = 0; n < ola_window_size_; ++n) {
        ch_opt[n] = ch_opt[n] * transition_window_[n] +
                    ch_target[n] * transition_window_[ola_window_size_ + n];
      }
    }
  }

  // Next target is one hop ahead of the current optimal.
  target_block_index_ = optimal_index + ola_hop_size_;
}

void AudioRendererAlgorithm::PeekAudioWithZeroPrepend(int read_offset_frames,
                                                      AudioBus* dest) {
  int write_offset = 0;
  int num_frames_to_read = dest->frames();
  if (read_offset_frames < 0) {
    const int num_zero_frames_appended =
        std::min(-read_offset_frames, num_frames_to_read);
    read_offset_frames = 0;
    num_frames_to_read -= num_zero_frames_appended;
    write_offset = num_zero_frames_appended;
    dest->ZeroFrames(num_zero_frames_appended);
  }
  queue_.PeekFrames(num_frames_to_read, read_offset_frames, write_offset,
                    dest);
}

namespace wsola {

namespace {

bool InInterval(int n, Interval q) {
  return n >= q.first && n <= q.second;
}

float MultiChannelSimilarityMeasure(const float* dot_prod_a_b,
                                    const float* energy_a,
                                    const float* energy_b,
                                    int channels) {
  const float kEpsilon = 1e-12f;
  float similarity_measure = 0.0f;
  for (int n = 0; n < channels; ++n) {
    similarity_measure +=
        dot_prod_a_b[n] / std::sqrt(energy_a[n] * energy_b[n] + kEpsilon);
  }
  return similarity_measure;
}

}  // namespace

void MultiChannelDotProduct(const AudioBus& a,
                            int frame_offset_a,
                            const AudioBus& b,
                            int frame_offset_b,
                            int num_frames,
                            float* dot_product) {
  for (int k = 0; k < a.channels(); ++k) {
    const float* ch_a = a.channel(k) + frame_offset_a;
    const float* ch_b = b.channel(k) + frame_offset_b;
    dot_product[k] = std::inner_product(ch_a, ch_a + num_frames, ch_b, 0.0f);
  }
}

void MultiChannelMovingBlockEnergies(const AudioBus& input,
                                     int frames_per_block,
                                     float* energy) {
  const int num_blocks = input.frames() - (frames_per_block - 1);
  const int channels = input.channels();

  for (int k = 0; k < channels; ++k) {
    const float* input_channel = input.channel(k);

    energy[k] = std::inner_product(input_channel,
                                   input_channel + frames_per_block,
                                   input_channel, 0.0f);
    const float* slide_out = input_channel;
    const float* slide_in = input_channel + frames_per_block;
    for (int n = 1; n < num_blocks; ++n, ++slide_in, ++slide_out) {
      energy[k + n * channels] = energy[k + (n - 1) * channels] -
                                 *slide_out * *slide_out +
                                 *slide_in * *slide_in;
    }
  }
}

void QuadraticInterpolation(const float* y_values,
                            float* extremum,
                            float* extremum_value) {
  const float a = 0.5f * (y_values[2] + y_values[0]) - y_values[1];
  const float b = 0.5f * (y_values[2] - y_values[0]);
  const float c = y_values[1];

  if (a == 0.f) {
    // The coordinates are colinear (within floating-point error).
    *extremum = 0;
    *extremum_value = y_values[1];
  } else {
    *extremum = -b / (2.f * a);
    *extremum_value = a * *extremum * *extremum + b * *extremum + c;
  }
}

int DecimatedSearch(int decimation,
                    Interval exclude_interval,
                    const AudioBus& target_block,
                    const AudioBus& search_segment,
                    const float* energy_target_block,
                    const float* energy_candidate_blocks) {
  const int channels = search_segment.channels();
  const int block_size = target_block.frames();
  const int num_candidate_blocks = search_segment.frames() - (block_size - 1);
  std::vector<float> dot_prod(channels);
  float similarity[3];  // Three elements for cubic interpolation.

  int n = 0;
  MultiChannelDotProduct(target_block, 0, search_segment, n, block_size,
                         dot_prod.data());
  similarity[0] = MultiChannelSimilarityMeasure(
      dot_prod.data(), energy_target_block,
      &energy_candidate_blocks[static_cast<size_t>(n) * channels], channels);

  // Set the starting point as optimal point.
  float best_similarity = similarity[0];
  int optimal_index = 0;

  n += decimation;
  if (n >= num_candidate_blocks) {
    return 0;
  }

  MultiChannelDotProduct(target_block, 0, search_segment, n, block_size,
                         dot_prod.data());
  similarity[1] = MultiChannelSimilarityMeasure(
      dot_prod.data(), energy_target_block,
      &energy_candidate_blocks[static_cast<size_t>(n) * channels], channels);

  n += decimation;
  if (n >= num_candidate_blocks) {
    // We cannot do any more sampling. Compare these two values and return the
    // optimal index.
    return similarity[1] > similarity[0] ? decimation : 0;
  }

  for (; n < num_candidate_blocks; n += decimation) {
    MultiChannelDotProduct(target_block, 0, search_segment, n, block_size,
                           dot_prod.data());
    similarity[2] = MultiChannelSimilarityMeasure(
        dot_prod.data(), energy_target_block,
        &energy_candidate_blocks[static_cast<size_t>(n) * channels], channels);

    if ((similarity[1] > similarity[0] && similarity[1] >= similarity[2]) ||
        (similarity[1] >= similarity[0] && similarity[1] > similarity[2])) {
      // A local maximum is found. Do a cubic interpolation for a better
      // estimate of candidate maximum.
      float normalized_candidate_index;
      float candidate_similarity;
      QuadraticInterpolation(similarity, &normalized_candidate_index,
                             &candidate_similarity);

      // The offset from the middle point lies in [-0.5, 0.5] and is often
      // negative, where adding 0.5 and truncating rounds toward zero rather
      // than to nearest. Chromium's algorithm does this; keep it to pick the
      // same candidate.
      // NOLINTBEGIN(bugprone-incorrect-roundings)
      const int candidate_index =
          n - decimation +
          static_cast<int>(normalized_candidate_index * decimation + 0.5f);
      // NOLINTEND(bugprone-incorrect-roundings)
      if (candidate_similarity > best_similarity &&
          !InInterval(candidate_index, exclude_interval)) {
        optimal_index = candidate_index;
        best_similarity = candidate_similarity;
      }
    } else if (n + decimation >= num_candidate_blocks &&
               similarity[2] > best_similarity &&
               !InInterval(n, exclude_interval)) {
      // If this is the end-point and has a better similarity-measure than
      // optimal, then we accept it as optimal point.
      optimal_index = n;
      best_similarity = similarity[2];
    }

    similarity[0] = similarity[1];
    similarity[1] = similarity[2];
  }
  return optimal_index;
}

int FullSearch(int low_limit,
               int high_limit,
               Interval exclude_interval,
               const AudioBus& target_block,
               const AudioBus& search_block,
               const float* energy_target_block,
               const float* energy_candidate_blocks) {
  const int channels = search_block.channels();
  const int block_size = target_block.frames();
  std::vector<float> dot_prod(channels);

  float best_similarity = std::numeric_limits<float>::lowest();
  int optimal_index = 0;

  for (int n = low_limit; n <= high_limit; ++n) {
    if (InInterval(n, exclude_interval)) {
      continue;
    }
    MultiChannelDotProduct(target_block, 0, search_block, n, block_size,
                           dot_prod.data());
    const float similarity = MultiChannelSimilarityMeasure(
        dot_prod.data(), energy_target_block,
        &energy_candidate_blocks[static_cast<size_t>(n) * channels], channels);
    if (similarity > best_similarity) {
      best_similarity = similarity;
      optimal_index = n;
    }
  }
  return optimal_index;
}

int OptimalIndex(const AudioBus& search_block,
                 const AudioBus& target_block,
                 Interval exclude_interval) {
  const int channels = search_block.channels();
  const int target_size = target_block.frames();
  const int num_candidate_blocks = search_block.frames() - (target_size - 1);

  // This is a compromise between complexity reduction and search accuracy. I
  // don't have a proof that down sample of order 5 is optimal. One can compute
  // a decimation factor that minimizes complexity given the size of
  // |search_block| and |target_block|. However, my experiments show the rate of
  // missing the optimal index is significant. This value is chosen
  // heuristically based on experiments.
  constexpr int kSearchDecimation = 5;

  std::vector<float> energy_target_block(channels);
  std::vector<float> energy_candidate_blocks(static_cast<size_t>(channels) *
                                             num_candidate_blocks);

  // Energy of all candid frames.
  MultiChannelMovingBlockEnergies(search_block, target_size,
                                  energy_candidate_blocks.data());

  // Energy of target frame.
  MultiChannelDotProduct(target_block, 0, target_block, 0, target_size,
                         energy_target_block.data());

  const int optimal_index = DecimatedSearch(
      kSearchDecimation, exclude_interval, target_block, search_block,
      energy_target_block.data(), energy_candidate_blocks.data());

  const int lim_low = std::max(0, optimal_index - kSearchDecimation);
  const int lim_high =
      std::min(num_candidate_blocks - 1, optimal_index + kSearchDecimation);
  return FullSearch(lim_low, lim_high, exclude_interval, target_block,
                    search_block, energy_target_block.data(),
                    energy_candidate_blocks.data());
}

void GetPeriodicHanningWindow(std::vector<float>* window) {
  const float scale =
      2.0f * std::numbers::pi_v<float> / static_cast<float>(window->size());
  for (size_t n = 0; n < window->size(); ++n) {
    (*window)[n] = 0.5f * (1.0f - std::cos(static_cast<float>(n) * scale));
  }
}

}  // namespace wsola

}  // namespace lychee
