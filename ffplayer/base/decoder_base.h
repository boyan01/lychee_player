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

extern AVPacket *flush_pkt;

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

#endif //FFP_DECODER_BASE_H
