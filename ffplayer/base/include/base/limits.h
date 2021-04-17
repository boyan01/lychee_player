//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_BASE_LIMITS_H_
#define MEDIA_BASE_LIMITS_H_

namespace media {
namespace limits {

enum {
  // Maximum possible dimension (width or height) for any video.
  kMaxDimension = (1 << 15) - 1,  // 32767

  // Maximum possible canvas size (width multiplied by height) for any video.
  kMaxCanvas = (1 << (14 * 2)),  // 16384 x 16384

  // Total number of video frames which are populating in the pipeline.
  kMaxVideoFrames = 4,

  // The following limits are used by AudioParameters::IsValid().
  //
  // A few notes on sample rates of common formats:
  //   - AAC files are limited to 96 kHz.
  //   - MP3 files are limited to 48 kHz.
  //   - Vorbis used to be limited to 96 KHz, but no longer has that
  //     restriction.
  //   - Most PC audio hardware is limited to 192 KHz.
  kMaxSampleRate = 192000,
  kMinSampleRate = 8000,
  kMaxChannels = 32,
  kMaxBitsPerSample = 64,
  kMaxBytesPerSample = kMaxBitsPerSample / 8,
  kMaxSamplesPerPacket = kMaxSampleRate,
  kMaxPacketSizeInBytes =
  (kMaxBitsPerSample / 8) * kMaxChannels * kMaxSamplesPerPacket,

};

} // namespace limits
} // namespace media

#endif //MEDIA_BASE_LIMITS_H_
