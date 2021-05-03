//
// Created by yangbin on 2021/5/1.
//

#include "base/logging.h"
#include "base/lambda.h"
#include "base/bind_to_current_loop.h"

#include "audio_renderer.h"

namespace media {

AudioRenderer::AudioRenderer(TaskRunner *task_runner, std::shared_ptr<AudioRendererSink> sink)
    : task_runner_(task_runner), audio_buffer_(3), sink_(std::move(sink)) {}

AudioRenderer::~AudioRenderer() {

}

void AudioRenderer::Initialize(DemuxerStream *stream,
                               std::shared_ptr<MediaClock> media_clock,
                               InitCallback init_callback) {
  task_runner_ = TaskRunner::current();
  DCHECK(task_runner_);
  media_clock_ = std::move(media_clock);
  demuxer_stream_ = stream;
  init_callback_ = BindToCurrentLoop(std::move(init_callback));

  decoder_stream_ = std::make_shared<AudioDecoderStream>(std::make_unique<AudioDecoderStream::StreamTraits>(),
                                                         task_runner_);

  decoder_stream_->Initialize(stream, bind_weak(&AudioRenderer::OnDecoderStreamInitialized,
                                                shared_from_this()));
}

void AudioRenderer::OnDecoderStreamInitialized(bool success) {
  DLOG(INFO) << __func__ << ": " << success;
  DCHECK(task_runner_->BelongsToCurrentThread());

  if (!success) {
    init_callback_(false);
    init_callback_ = nullptr;
    return;
  }

  auto audio_config = demuxer_stream_->audio_decode_config();
  sink_->Initialize(
      av_get_channel_layout_nb_channels(audio_config.channel_layout()),
      audio_config.samples_per_second(),
      this);

  init_callback_(true);
  init_callback_ = nullptr;

  task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));

}

void AudioRenderer::AttemptReadFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  decoder_stream_->Read(bind_weak(&AudioRenderer::OnNewFrameAvailable, shared_from_this()));
}

void AudioRenderer::OnNewFrameAvailable(AudioDecoderStream::ReadResult result) {
  audio_buffer_.InsertLast(std::move(result));
}

int AudioRenderer::Render(double delay, uint8 *stream, int len) {

  return 0;
}

void AudioRenderer::OnRenderError() {

}

void AudioRenderer::Start() {

}

void AudioRenderer::Stop() {

}

}