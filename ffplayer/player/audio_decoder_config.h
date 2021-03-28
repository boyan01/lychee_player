//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_
#define MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_

#include "basictypes.h"
#include "channel_layout.h"

namespace media {

enum AudioCodec {
  // These values are histogrammed over time; do not change their ordinal
  // values.  When deleting a codec replace it with a dummy value; when adding a
  // codec, do so at the bottom (and update kAudioCodecMax).
  kUnknownAudioCodec = 0,
  kCodecAAC,
  kCodecMP3,
  kCodecPCM,
  kCodecVorbis,
  // ChromiumOS and ChromeOS specific codecs.
  kCodecFLAC,
  // ChromeOS specific codecs.
  kCodecAMR_NB,
  kCodecAMR_WB,
  kCodecPCM_MULAW,
  kCodecGSM_MS,
  kCodecPCM_S16BE,
  kCodecPCM_S24BE,
  // DO NOT ADD RANDOM AUDIO CODECS!
  //
  // The only acceptable time to add a new codec is if there is production code
  // that uses said codec in the same CL.

  kAudioCodecMax = kCodecPCM_S24BE  // Must equal the last "real" codec above.
};

//// TODO: FFmpeg API uses |bytes_per_channel| instead of
//// |bits_per_channel|, we should switch over since bits are generally confusing
//// to work with.
class AudioDecoderConfig {

 public:
  // Constructs an uninitialized object. Clients should call Initialize() with
  // appropriate values before using.
  AudioDecoderConfig();

  // Constructs an initialized object. It is acceptable to pass in NULL for
  // |extra_data|, otherwise the memory is copied.
  AudioDecoderConfig(AudioCodec codec, int bits_per_channel,
                     ChannelLayout channel_layout, int samples_per_second,
                     const uint8 *extra_data, size_t extra_data_size);

  ~AudioDecoderConfig();

  // Resets the internal state of this object.
  void Initialize(AudioCodec codec, int bits_per_channel,
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

  AudioCodec codec() const;
  int bits_per_channel() const;
  ChannelLayout channel_layout() const;
  int samples_per_second() const;

  // Optional byte data required to initialize audio decoders such as Vorbis
  // codebooks.
  uint8 *extra_data() const;
  size_t extra_data_size() const;

 private:
  AudioCodec codec_;
  int bits_per_channel_;
  ChannelLayout channel_layout_;
  int samples_per_second_;

  std::shared_ptr<uint8[]> extra_data_;
  size_t extra_data_size_;

  DISALLOW_COPY_AND_ASSIGN(AudioDecoderConfig);

};

}

#endif //MEDIA_PLAYER_AUDIO_DECODER_CONFIG_H_
