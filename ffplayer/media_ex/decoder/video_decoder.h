//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_PLAYER_DECODER_VIDEO_DECODER_H_
#define MEDIA_PLAYER_DECODER_VIDEO_DECODER_H_

#include "functional"
#include "string"

#include "base/basictypes.h"
#include "decoder/video_decoder_config.h"
#include "decoder/decoder_buffer.h"
#include "decoder/decode_status.h"
#include "decoder/video_frame.h"

#include "ffmpeg/ffmpeg_deleters.h"
#include "ffmpeg/ffmpeg_decoding_loop.h"

namespace media {

class VideoDecoder {

 public:

  // Callback for VideoDecoder to return a decoded frame whenever it becomes
  // available. Only non-EOS frames should be returned via this callback.
  using OutputCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

  using DecodeCallback = std::function<void(DecodeStatus)>;

  using DecodeInitCallback = std::function<void(bool)>;

  VideoDecoder();

  virtual ~VideoDecoder();

  void Initialize(const VideoDecoderConfig &decoder_config,
                  bool low_delay,
                  const OutputCallback &output_callback,
                  DecodeInitCallback init_callback);

  void Decode(const std::shared_ptr<DecoderBuffer>& buffer, const DecodeCallback& decode_callback);

  virtual std::string GetDisplayName();

 private:

  enum DecoderState {
    kUninitialized,
    kNormal,
    kDecodeFinished,
    kError
  };

  DecoderState state_ = kUninitialized;

  // FFmpeg structures owned by this object.
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context_;

  std::unique_ptr<FFmpegDecodingLoop> decoding_loop_;

  OutputCallback output_cb_;

  VideoDecoderConfig config_;

  // Handles decoding of an unencrypted encoded buffer. A return value of false
  // indicates that an error has occurred.
  bool FFmpegDecode(const DecoderBuffer &buffer);
  bool OnNewFrame(AVFrame *frame);

  // Handles (re-)initializing the decoder with a (new) config.
  // Returns true if initialization was successful.
  bool ConfigureDecoder(const VideoDecoderConfig &config, bool low_delay);

  void ReleaseFFmpegResources();

  DISALLOW_COPY_AND_ASSIGN(VideoDecoder);

};

} // namespace media

#endif //MEDIA_PLAYER_DECODER_VIDEO_DECODER_H_
