//
// Created by boyan on 2021/2/14.
//
#include "decoder_base.h"

#include <utility>

DecodeParams::DecodeParams(std::shared_ptr<PacketQueue> pkt_queue_,
                           std::shared_ptr<std::condition_variable_any> read_condition_,
                           AVFormatContext *const *format_ctx_, int stream_index_)
    : pkt_queue(std::move(pkt_queue_)),
      read_condition(std::move(read_condition_)),
      format_ctx(format_ctx_),
      stream_index(stream_index_) {
}

AVStream *DecodeParams::stream() const {
  if (!*format_ctx) {
    return nullptr;
  } else {
    return (*format_ctx)->streams[stream_index];
  }
}

Decoder::Decoder(
    unique_ptr_d<AVCodecContext> codec_context,
    std::unique_ptr<DecodeParams> decode_params_,
    std::function<void()> on_decoder_blocking
) : decode_params(std::move(decode_params_)),
    avctx(std::move(codec_context)),
    on_decoder_blocking_(std::move(on_decoder_blocking)) {
}

Decoder::~Decoder() {
  if (decoder_tid) {
    av_log(nullptr, AV_LOG_WARNING, "decoder destroyed but thread do not complete.\n");
  }
}

int Decoder::DecodeFrame(AVFrame *frame, AVSubtitle *sub) {
  auto *d = this;
  int ret = AVERROR(EAGAIN);

  while (!abort_decoder) {
    AVPacket temp_pkt;

    if (d->queue()->serial == d->pkt_serial) {
      do {
        if (d->queue()->abort_request || abort_decoder)
          return -1;

        switch (d->avctx->codec_type) {
          case AVMEDIA_TYPE_VIDEO: {
            ret = avcodec_receive_frame(d->avctx.get(), frame);
            if (ret >= 0) {
              if (d->decoder_reorder_pts == -1) {
                frame->pts = frame->best_effort_timestamp;
              } else if (!d->decoder_reorder_pts) {
                frame->pts = frame->pkt_dts;
              }
            }
            break;
          }
          case AVMEDIA_TYPE_AUDIO: {
            ret = avcodec_receive_frame(d->avctx.get(), frame);
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
          }
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
        NotifyQueueEmpty();
      }
      if (d->packet_pending) {
        av_packet_move_ref(&temp_pkt, &d->pkt);
        d->packet_pending = 0;
      } else {

        while (!abort_decoder && !queue()->abort_request && queue()->DequeuePacket(temp_pkt, &d->pkt_serial) < 0) {
          std::unique_lock<std::mutex> lock(queue()->mutex);
          av_log(nullptr, AV_LOG_DEBUG, "%s pending for waiting packets.\n", debug_label());
          if (on_decoder_blocking_) {
            on_decoder_blocking_();
          }
          queue()->cond.wait(lock);
        }

        if (abort_decoder || queue()->abort_request) {
          return -1;
        }
      }
      if (d->queue()->serial == d->pkt_serial) {
        // we got the correct pkt.
        break;
      } else {
        av_packet_unref(&temp_pkt);
      }
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
  return 0;
}

void Decoder::Start() {
  decode_params->pkt_queue->Start();
  decoder_tid = new std::thread([this]() {
    update_thread_name(debug_label());
    av_log(nullptr, AV_LOG_INFO, "start decoder thread: %s.\n", debug_label());
    int ret = DecodeThread();
    av_log(nullptr, AV_LOG_INFO, "thread: %s done. ret = %d\n", debug_label(), ret);
  });
}

void Decoder::Abort(FrameQueue *fq) {
  abort_decoder = true;
  queue()->Abort();
  AbortRender();
  queue()->Flush();
  if (fq) {
    fq->Signal();
  }
}

void Decoder::Join() {
  if (decoder_tid && decoder_tid->joinable()) {
    decoder_tid->join();
    decoder_tid = nullptr;
  }
}
