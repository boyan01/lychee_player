//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_
#define MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_

#include "map"

#include "demuxer/demuxer_stream.h"
#include "decoder/video_frame.h"
#include "decoder/video_decoder.h"
#include "pipeline/pipeline_status.h"

namespace media {

template<DemuxerStream::Type StreamType>
class DecoderStreamTraits {};

enum class PostDecodeAction { DELIVER, DROP };

template<>
class DecoderStreamTraits<DemuxerStream::VIDEO>
    : public std::enable_shared_from_this<DecoderStreamTraits<DemuxerStream::VIDEO>> {
 public:
  using OutputType = VideoFrame;
  using DecoderType = VideoDecoder;
  using DecoderConfigType = VideoDecoderConfig;
  using InitCB = VideoDecoder::DecodeInitCallback;
  using OutputCB = VideoDecoder::OutputCallback;

  static std::string ToString();
  static bool NeedsBitstreamConversion(DecoderType *decoder);

  explicit DecoderStreamTraits();

  DecoderConfigType GetDecoderConfig(DemuxerStream *stream);
  void SetIsPlatformDecoder(bool is_platform_decoder);
  void SetIsDecryptingDemuxerStream(bool is_dds);
  void InitializeDecoder(DecoderType *decoder,
                         const DecoderConfigType &config,
                         bool low_delay,
                         InitCB init_cb,
                         const OutputCB &output_cb);
  void OnDecoderInitialized(DecoderType *decoder, InitCB cb, bool ok);
  void OnDecode(const DecoderBuffer &buffer);
  PostDecodeAction OnDecodeDone(OutputType *buffer);
  void OnStreamReset(DemuxerStream *stream);
  void OnOutputReady(OutputType *output);

 private:
  TimeDelta last_keyframe_timestamp_;
//  MovingAverage keyframe_distance_average_;

  // Tracks the duration of incoming packets over time.
  struct FrameMetadata {
    bool should_drop = false;
    TimeDelta duration = kNoTimestamp();
    TimeDelta decode_begin_time = kNoTimestamp();
  };
  std::map<TimeDelta, FrameMetadata> frame_metadata_;

  PipelineStatistics stats_;

};

} // namespace media

#endif //MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_
