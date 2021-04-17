//
// Created by yangbin on 2021/4/5.
//

#include "renderer/video_renderer.h"

#include "base/logging.h"

namespace media {

void VideoRenderer::Initialize(DemuxerStream *audio_stream, PipelineStatusCallback init_callback) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  std::lock_guard<std::mutex> auto_lock(mutex_);
  DCHECK(audio_stream);
  DCHECK_EQ(audio_stream->type(), DemuxerStream::VIDEO);
  DCHECK(init_callback);
  DCHECK(kUninitialized == state_ || kFlushed == state_);

  demuxer_stream_ = audio_stream;
  video_decoder_stream_ = std::make_unique<VideoDecoderStream>(
      std::make_unique<VideoDecoderStream::StreamTraits>(),
      task_runner_
  );

  init_cb_ = std::move(init_callback);

}

}