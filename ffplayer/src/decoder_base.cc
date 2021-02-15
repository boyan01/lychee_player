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