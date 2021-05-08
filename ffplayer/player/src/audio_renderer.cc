//
// Created by yangbin on 2021/5/1.
//

#include "base/logging.h"
#include "base/lambda.h"
#include "base/bind_to_current_loop.h"

#include "ffp_utils.h"

#include "audio_renderer.h"

namespace media {

AudioRenderer::AudioRenderer(TaskRunner *task_runner, std::shared_ptr<AudioRendererSink> sink)
    : task_runner_(task_runner), audio_buffer_(3), sink_(std::move(sink)) {}

AudioRenderer::~AudioRenderer() {

}

void AudioRenderer::Initialize(DemuxerStream *stream,
                               std::shared_ptr<MediaClock> media_clock,
                               InitCallback init_callback) {
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
    auto callback = std::move(init_callback_);
    init_callback_ = nullptr;
    std::move(callback)(false);
    return;
  }

  auto audio_config = demuxer_stream_->audio_decode_config();
  sink_->Initialize(
      av_get_channel_layout_nb_channels(audio_config.channel_layout()),
      audio_config.samples_per_second(),
      this);

  auto callback = std::move(init_callback_);
  init_callback_ = nullptr;
  std::move(callback)(true);

  task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));

}

void AudioRenderer::AttemptReadFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());

  if (!NeedReadStream() || reading_) {
    return;
  }

  reading_ = true;
  decoder_stream_->Read(bind_weak(&AudioRenderer::OnNewFrameAvailable, shared_from_this()));
}

void AudioRenderer::OnNewFrameAvailable(AudioDecoderStream::ReadResult result) {
  reading_ = false;
  DLOG_IF(WARNING, audio_buffer_.IsFull()) << "OnNewFrameAvailable is full";
  audio_buffer_.InsertLast(std::move(result));
  if (NeedReadStream()) {
    AttemptReadFrame();
  }
}

int AudioRenderer::Render(double delay, uint8 *stream, int len) {

  DCHECK_GT(len, 0);
  DCHECK(stream);

  double audio_clock_time = 0;
  auto render_callback_time = get_relative_time();

  auto len_flush = 0;
  while (len_flush < len) {
    if (audio_buffer_.IsEmpty()) {
      break;
    }
    auto buffer = audio_buffer_.GetFront();
    DCHECK(buffer);
    if (audio_clock_time == 0) {
//      TODO It is like we do not need calculate delay for clock time.
      audio_clock_time = buffer->PtsFromCursor() - delay;
//      audio_clock_time = buffer->PtsFromCursor();
    }

    auto flushed = buffer->Read(stream + len_flush, len - len_flush);
    if (buffer->IsConsumed()) {
      audio_buffer_.DeleteFront();
      task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));
    }
    len_flush += flushed;
  }

  if (audio_clock_time != 0) {
    media_clock_->GetAudioClock()->SetClockAt(audio_clock_time, 0, render_callback_time);
    media_clock_->GetExtClock()->Sync(media_clock_->GetAudioClock());
  }

//  task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));

  return len_flush;
}

void AudioRenderer::OnRenderError() {

}

void AudioRenderer::Start() {
  sink_->Play();
}

void AudioRenderer::Stop() {

}

bool AudioRenderer::NeedReadStream() {
  // FIXME temp solution.
  return !audio_buffer_.IsFull();
}

}