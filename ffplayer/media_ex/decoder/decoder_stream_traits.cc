//
// Created by yangbin on 2021/4/8.
//

#include "decoder_stream_traits.h"

#include "base/logging.h"
#include "base/lambda.h"

namespace media {

// Video decoder stream traits implementation.

// static
std::string DecoderStreamTraits<DemuxerStream::VIDEO>::ToString() {
  return "video";
}

// static
bool DecoderStreamTraits<DemuxerStream::VIDEO>::NeedsBitstreamConversion(
    DecoderType *decoder) {
  return false;
}

void DecoderStreamTraits<DemuxerStream::VIDEO>::SetIsPlatformDecoder(
    bool is_platform_decoder) {
  stats_.video_decoder_info.is_platform_decoder = is_platform_decoder;
}

void DecoderStreamTraits<DemuxerStream::VIDEO>::SetIsDecryptingDemuxerStream(
    bool is_dds) {
  stats_.video_decoder_info.has_decrypting_demuxer_stream = is_dds;
}

DecoderStreamTraits<DemuxerStream::VIDEO>::DecoderStreamTraits() {

}

void DecoderStreamTraits<DemuxerStream::VIDEO>::InitializeDecoder(
    DecoderType *decoder,
    const DecoderConfigType &config,
    bool low_delay,
    InitCB init_cb,
    const OutputCB &output_cb) {
  DCHECK(config.IsValidConfig());
  // |decoder| is owned by a DecoderSelector and will stay
  // alive at least until |init_cb| is finished executing.
  decoder->Initialize(
      config, low_delay,
      output_cb,
      [WEAK_THIS(DecoderStreamTraits<DemuxerStream::VIDEO>), decoder, callback(std::move(init_cb))](bool init) {
        auto traits = weak_this.lock();
        if (traits) {
          traits->OnDecoderInitialized(decoder, callback, false);
        }
      });
}

void DecoderStreamTraits<DemuxerStream::VIDEO>::OnDecoderInitialized(DecoderType *decoder, InitCB cb, bool ok) {
  if (ok) {
//    stats_.video_decoder_info.decoder_type = decoder->GetDecoderType();
    stats_.video_decoder_info.decoder_type = 0;
    DLOG(INFO) << stats_.video_decoder_info.decoder_type;
  } else {
    DLOG(INFO) << "Decoder initialization failed.";
  }
  std::move(cb)(ok);
}

void DecoderStreamTraits<DemuxerStream::VIDEO>::OnStreamReset(
    DemuxerStream *stream) {
  DCHECK(stream);
  last_keyframe_timestamp_ = TimeDelta();
  frame_metadata_.clear();
}

void DecoderStreamTraits<DemuxerStream::VIDEO>::OnDecode(
    const DecoderBuffer &buffer) {
  if (buffer.isEndOfStream()) {
    last_keyframe_timestamp_ = TimeDelta();
    return;
  }

//  frame_metadata_[buffer.GetTimestamp()] = {
////      buffer.discard_padding().first == kInfiniteDuration,  // should_drop
//      false,
//      buffer.GetDuration(),                                    // duration
//      chrono::system_clock::now(),                               // decode_begin_time
//  };

//  if (!buffer.is_key_frame())
//    return;

  TimeDelta current_frame_timestamp = buffer.GetTimestamp();
  if (last_keyframe_timestamp_ == kNoTimestamp()) {
    last_keyframe_timestamp_ = current_frame_timestamp;
    return;
  }

  const TimeDelta frame_distance =
      current_frame_timestamp - last_keyframe_timestamp_;
  last_keyframe_timestamp_ = current_frame_timestamp;
//  keyframe_distance_average_.AddSample(frame_distance);
}

PostDecodeAction DecoderStreamTraits<DemuxerStream::VIDEO>::OnDecodeDone(
    OutputType *buffer) {
////  auto it = frame_metadata_.find(buffer->timestamp());
//
//  // If the frame isn't in |frame_metadata_| it probably was erased below on a
//  // previous cycle. We could drop these, but today our video algorithm will put
//  // them back into sorted order or drop the frame if a later frame has already
//  // been rendered.
//  if (it == frame_metadata_.end())
//    return PostDecodeAction::DELIVER;
//
//  // Add a timestamp here to enable buffering delay measurements down the line.
//  buffer->metadata().decode_begin_time = it->second.decode_begin_time;
//  buffer->metadata().decode_end_time = base::TimeTicks::Now();
//
//  auto action = it->second.should_drop ? PostDecodeAction::DROP
//                                       : PostDecodeAction::DELIVER;
//
//  // Provide duration information to help the rendering algorithm on the very
//  // first and very last frames.
//  if (it->second.duration != kNoTimestamp)
//    buffer->metadata().frame_duration = it->second.duration;
//
//  // We erase from the beginning onward to our target frame since frames should
//  // be returned in presentation order. It's possible to accumulate entries in
//  // this queue if playback begins at a non-keyframe; those frames may never be
//  // returned from the decoder.
//  frame_metadata_.erase(frame_metadata_.begin(), it + 1);
//  return action;

  return PostDecodeAction::DELIVER;

}

void DecoderStreamTraits<DemuxerStream::VIDEO>::OnOutputReady(
    OutputType *buffer) {
//  buffer->metadata().transformation = transform_;
//
//  if (!buffer->metadata().decode_begin_time.has_value())
//    return;
//
//  // Tag buffer with elapsed time since creation.
//  buffer->metadata().processing_time =
//      base::TimeTicks::Now() - *buffer->metadata().decode_begin_time;
}

} // namespace media