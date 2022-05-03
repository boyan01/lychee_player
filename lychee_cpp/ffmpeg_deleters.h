//
// Created by yangbin on 2021/4/6.
//

#ifndef MEDIA_PLAYER_FFMPEG_FFMPEG_DELETERS_H_
#define MEDIA_PLAYER_FFMPEG_FFMPEG_DELETERS_H_

#include "memory"

extern "C" {
#include "libavformat/avformat.h"
};

namespace lychee {

#define ffmpeg_unique_ptr(type) std::unique_ptr<type, ##type##Deleter>

// Frees an AVCodecContext object in a class that can be passed as a Deleter
// argument to scoped_ptr_malloc.
struct AVCodecContextDeleter {
  void operator()(void *x) const {
    auto *codec_context = static_cast<AVCodecContext *>(x);
    avcodec_free_context(&codec_context);
  }
};

// Frees an AVFrame object in a class that can be passed as a Deleter argument
// to scoped_ptr_malloc.
struct ScopedPtrAVFreeFrame {
  inline void operator()(void *x) const {
    auto *frame = static_cast<AVFrame *>(x);
    av_frame_free(&frame);
  }
};

struct AVPacketDeleter {
  void operator()(void *x) const {
    auto *packet = static_cast<AVPacket *>(x);
    av_packet_unref(packet);
    delete packet;
  }
};

}  // namespace media

#endif  // MEDIA_PLAYER_FFMPEG_FFMPEG_DELETERS_H_
