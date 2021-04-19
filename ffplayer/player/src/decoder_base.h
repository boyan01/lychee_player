//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_BASE_H
#define FFP_DECODER_BASE_H

#include <functional>
#include <thread>
#include <condition_variable>
#include <memory>

#include "ffp_utils.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "ffp_packet_queue.h"
#include "ffp_frame_queue.h"
#include "ffp_define.h"

#include "render_base.h"

namespace media {

class VideoDecodeConfig {

 public:

  VideoDecodeConfig() : codec_parameters_(), time_base_(), frame_rate_(), max_frame_duration_(0) {
    memset(&codec_parameters_, 0, sizeof(AVCodecParameters));
    memset(&time_base_, 0, sizeof(AVRational));
    memset(&frame_rate_, 0, sizeof(AVRational));
  }

  VideoDecodeConfig(const AVCodecParameters &codec_parameters,
                    const AVRational time_base,
                    const AVRational frame_rate,
                    double max_frame_duration)
      : codec_parameters_(codec_parameters),
        time_base_(time_base),
        frame_rate_(frame_rate),
        max_frame_duration_(max_frame_duration) {}

  AVCodecID codec_id() const {
    return codec_parameters_.codec_id;
  }

  const AVCodecParameters &codec_parameters() const {
    return codec_parameters_;
  }

  const AVRational &time_base() const {
    return time_base_;
  }

  const AVRational &frame_rate() const {
    return frame_rate_;
  }

  /**
   * 1 -> 1/2  2-> 1/4
   */
  int low_res() const {
    return 0;
  }

  bool fast() const {
    return false;
  }

  double max_frame_duration() const {
    return max_frame_duration_;
  }

 private:
  AVCodecParameters codec_parameters_;
  AVRational time_base_;
  AVRational frame_rate_;
  double max_frame_duration_;

};

struct DecodeParams {
  std::shared_ptr<PacketQueue> pkt_queue;
  // Use to notify data_source if decoder have not enough packets.
  std::shared_ptr<std::condition_variable_any> read_condition;
  AVFormatContext *const *format_ctx;
  int stream_index = -1;
  bool audio_follow_stream_start_pts = false;

 public:
  DecodeParams(
      std::shared_ptr<PacketQueue> pkt_queue_,
      std::shared_ptr<std::condition_variable_any> read_condition_,
      AVFormatContext *const *format_ctx_,
      int stream_index_
  );

  AVStream *stream() const;

};

class Decoder {

 private:
  // Callback invoked when decode blocking because lack of packets.
  std::function<void()> on_decoder_blocking_;

 protected:
  bool abort_decoder = false;

 public:
  AVPacket pkt{0};
  unique_ptr_d<AVCodecContext> avctx;
  int pkt_serial = -1;
  int finished = 0;
  int packet_pending = 0;
  int64_t start_pts = AV_NOPTS_VALUE;
  AVRational start_pts_tb{0};
  int64_t next_pts = 0;
  AVRational next_pts_tb{};
  std::thread *decoder_tid = nullptr;
  int decoder_reorder_pts = -1;
  std::unique_ptr<DecodeParams> decode_params;

 protected:

  const std::shared_ptr<PacketQueue> &queue() const {
    return decode_params->pkt_queue;
  }

  void NotifyQueueEmpty() const {
    decode_params->read_condition->notify_all();
  }

  int DecodeFrame(AVFrame *frame, AVSubtitle *sub);

  virtual const char *debug_label() = 0;

  virtual int DecodeThread() = 0;

  virtual void AbortRender() = 0;

  void Start();

 public:

  Decoder(
      unique_ptr_d<AVCodecContext> codec_context,
      std::unique_ptr<DecodeParams> decode_params_,
      std::function<void()> on_decoder_blocking);

  virtual ~Decoder();

  void Abort(FrameQueue *fq);

  void Join();

};

}

#endif //FFP_DECODER_BASE_H
