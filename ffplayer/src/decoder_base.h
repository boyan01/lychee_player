//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_BASE_H
#define FFP_DECODER_BASE_H

#include <functional>
#include <condition_variable>
#include <memory>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "SDL2/SDL.h"
};

#include "ffp_packet_queue.h"
#include "ffp_frame_queue.h"
#include "ffp_define.h"

#include "render_base.h"
#include "ffp_audio_render.h"

struct DecodeParams {
  std::shared_ptr<PacketQueue> pkt_queue;
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

template<class T>
class Decoder {

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
  AVRational next_pts_tb;
  SDL_Thread *decoder_tid = nullptr;
  int decoder_reorder_pts = -1;
  std::function<void()> on_frame_decode_block = nullptr;
  std::shared_ptr<T> render;
  std::unique_ptr<DecodeParams> decode_params;

 private:

  int Start(int (*fn)(void *), void *arg) {
    decode_params->pkt_queue->Start();
    decoder_tid = SDL_CreateThread(fn, debug_label(), arg);
    if (!decoder_tid) {
      av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
      return AVERROR(ENOMEM);
    }
    return 0;
  }

 protected:

  const std::shared_ptr<PacketQueue> &queue() {
    return decode_params->pkt_queue;
  }

  const std::shared_ptr<std::condition_variable_any> &empty_queue_cond() {
    return decode_params->read_condition;
  }

  int DecodeFrame(AVFrame *frame, AVSubtitle *sub) {
    auto *d = this;
    int ret = AVERROR(EAGAIN);

    while (!abort_decoder) {
      AVPacket temp_pkt;

      if (d->queue()->serial == d->pkt_serial) {
        do {
          if (d->queue()->abort_request || abort_decoder)
            return -1;

          switch (d->avctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:ret = avcodec_receive_frame(d->avctx.get(), frame);
              if (ret >= 0) {
                if (d->decoder_reorder_pts == -1) {
                  frame->pts = frame->best_effort_timestamp;
                } else if (!d->decoder_reorder_pts) {
                  frame->pts = frame->pkt_dts;
                }
              }
              break;
            case AVMEDIA_TYPE_AUDIO:ret = avcodec_receive_frame(d->avctx.get(), frame);
              if (ret >= 0) {
                AVRational tb = {1, frame->sample_rate};
                if (frame->pts != AV_NOPTS_VALUE)
                  frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                else if (d->next_pts != AV_NOPTS_VALUE)
                  frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                if (frame->pts != AV_NOPTS_VALUE) {
                  d->next_pts = frame->pts + frame->nb_samples;
                  d->next_pts_tb = tb;
                }
              }
              break;
            default:break;
          }
          if (ret == AVERROR_EOF) {
            d->finished = d->pkt_serial;
            avcodec_flush_buffers(d->avctx.get());
            return 0;
          }
          if (ret >= 0)
            return 1;
        } while (ret != AVERROR(EAGAIN));
      }

      do {
        if (d->queue()->nb_packets == 0) {
          d->empty_queue_cond()->notify_all();
        }
        if (d->packet_pending) {
          av_packet_move_ref(&temp_pkt, &d->pkt);
          d->packet_pending = 0;
        } else {
          if (d->queue()->Get(&temp_pkt, 1, &d->pkt_serial, d, [](void *opacity) {
            auto decoder = static_cast<Decoder *>(opacity);
            if (decoder->on_frame_decode_block) {
              decoder->on_frame_decode_block();
            }
          }) < 0) {
            return -1;
          }
        }
        if (d->queue()->serial == d->pkt_serial)
          break;
        av_packet_unref(&temp_pkt);
      } while (true);

      if (temp_pkt.data == flush_pkt->data) {
        avcodec_flush_buffers(d->avctx.get());
        d->finished = 0;
        d->next_pts = d->start_pts;
        d->next_pts_tb = d->start_pts_tb;
      } else {
        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
          int got_frame = 0;
          ret = avcodec_decode_subtitle2(d->avctx.get(), sub, &got_frame, &temp_pkt);
          if (ret < 0) {
            ret = AVERROR(EAGAIN);
          } else {
            if (got_frame && !temp_pkt.data) {
              d->packet_pending = 1;
              av_packet_move_ref(&d->pkt, &temp_pkt);
            }
            ret = got_frame ? 0 : (temp_pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
          }
        } else {
          if (avcodec_send_packet(d->avctx.get(), &temp_pkt) == AVERROR(EAGAIN)) {
            av_log(d->avctx.get(), AV_LOG_ERROR,
                   "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
            d->packet_pending = 1;
            av_packet_move_ref(&d->pkt, &temp_pkt);
          }
        }
        av_packet_unref(&temp_pkt);
      }
    }
  }

  virtual const char *debug_label() = 0;

  virtual int DecodeThread() = 0;

  void Start() {
    Start([](void *arg) {
      auto *decoder = static_cast<Decoder<T> *>(arg);
      int ret = decoder->DecodeThread();
      av_log(nullptr, AV_LOG_INFO, "thread: %s done. \n", decoder->debug_label());
      return ret;
    }, this);
  }

 public:

  Decoder(unique_ptr_d<AVCodecContext> codec_context, std::unique_ptr<DecodeParams> decode_params_,
          std::shared_ptr<T> render_)
      : decode_params(std::move(decode_params_)), avctx(std::move(codec_context)),
        render(std::move(render_)) {
    static_assert(std::is_base_of<BaseRender, T>::value, "T must inherit from BaseRender");
  }

  ~Decoder() {
    if (decoder_tid) {
      av_log(nullptr, AV_LOG_WARNING, "decoder destroyed but thread do not complete.\n");
    }
  }

  void Abort(FrameQueue *fq) {
    abort_decoder = true;
    queue()->Abort();
    render->Abort();
    queue()->Flush();
    if (fq) {
      fq->Signal();
    }
  }

  void Join() {
    if (decoder_tid) {
      SDL_WaitThread(decoder_tid, nullptr);
      decoder_tid = nullptr;
    }
  }

};

#endif //FFP_DECODER_BASE_H
