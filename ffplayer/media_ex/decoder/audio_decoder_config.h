//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_
#define MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_

#include <memory>

#include "base/basictypes.h"
#include "base/channel_layout.h"

extern "C" {
#include "libavformat/avformat.h"
};

namespace media {

class AudioDecoderConfig {

 public:
  // Constructs an uninitialized object. Clients should call Initialize() with
  // appropriate values before using.
  AudioDecoderConfig();

  // Constructs an initialized object. It is acceptable to pass in NULL for
  // |extra_data|, otherwise the memory is copied.
  AudioDecoderConfig(AVCodecID codec_id, int bytes_per_channel,
                     ChannelLayout channel_layout, int samples_per_second,
                     const uint8 *extra_data, size_t extra_data_size);

  ~AudioDecoderConfig();

  // Resets the internal state of this object.
  void Initialize(AVCodecID codec_id, int bytes_per_channel,
                  ChannelLayout channel_layout, int samples_per_second,
                  const uint8 *extra_data, size_t extra_data_size,
                  bool record_stats);

  // Deep copies |audio_config|.
  void CopyFrom(const AudioDecoderConfig &audio_config);

  // Returns true if this object has appropriate configuration values, false
  // otherwise.
  bool IsValidConfig() const;

  // Returns true if all fields in |config| match this config.
  // Note: The contents of |extra_data_| are compared not the raw pointers.
  bool Matches(const AudioDecoderConfig &config) const;

  AVCodecID CodecId() const;
  int bytes_per_channel() const;
  ChannelLayout channel_layout() const;
  int samples_per_second() const;

  // Optional byte data required to initialize audio decoders such as Vorbis
  // codebooks.
  uint8 *extra_data() const;
  size_t extra_data_size() const;

 private:
  AVCodecID codec_;
  int bytes_per_channel_;
  ChannelLayout channel_layout_;
  int samples_per_second_;

  uint8 *extra_data_;
  size_t extra_data_size_;

};

}

#endif //MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_
