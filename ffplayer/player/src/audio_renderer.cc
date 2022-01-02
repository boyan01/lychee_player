//
// Created by yangbin on 2021/5/1.
//

#include "cmath"

#include "base/logging.h"
#include "base/lambda.h"
#include "base/bind_to_current_loop.h"

#include "ffmpeg_utils.h"

#include "audio_renderer.h"

namespace media {

AudioRenderer::AudioRenderer(std::shared_ptr<TaskRunner> task_runner, std::shared_ptr<AudioRendererSink> sink)
    : task_runner_(std::move(task_runner)),
      audio_buffer_(),
      sink_(std::move(sink)),
      volume_(1) {

}

AudioRenderer::~AudioRenderer() {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  sink_ = nullptr;
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

  std::move(init_callback_)(true);
  init_callback_ = nullptr;

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
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(reading_);
  reading_ = false;
  {
    std::lock_guard<std::mutex> auto_lock(mutex_);
    DLOG_IF(WARNING, audio_buffer_.size() > 3) << "audio buffer is enough: " << audio_buffer_.size();
    audio_buffer_.emplace_back(std::move(result));
  }
  if (NeedReadStream()) {
    AttemptReadFrame();
  }
}

int AudioRenderer::Render(double delay, uint8 *stream, int len) {

  DCHECK_GT(len, 0);
  DCHECK(stream);

  double audio_clock_time = 0;
  auto render_callback_time = av_gettime_relative() / 1000000.0;

  auto len_flush = 0;
  while (len_flush < len) {
    std::lock_guard<std::mutex> auto_lock(mutex_);
    if (audio_buffer_.empty()) {
      task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));
      break;
    }
    auto buffer = audio_buffer_.front();
    DCHECK(buffer);
    if (audio_clock_time == 0 && !std::isnan(buffer->pts())) {
      audio_clock_time = buffer->PtsFromCursor() - delay;
    }

    auto flushed = buffer->Read(stream + len_flush, len - len_flush, volume_);
    if (buffer->IsConsumed()) {
      audio_buffer_.pop_front();
      task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));
    }
    len_flush += flushed;
  }

  if (audio_clock_time != 0) {
    media_clock_->GetAudioClock()->SetClockAt(audio_clock_time, 0, render_callback_time);
    media_clock_->GetExtClock()->Sync(media_clock_->GetAudioClock());
  }

//  decode_task_runner_->PostTask(FROM_HERE, bind_weak(&AudioRenderer::AttemptReadFrame, shared_from_this()));

  return len_flush;
}

void AudioRenderer::OnRenderError() {

}

void AudioRenderer::Start() {
  sink_->Play();
}

void AudioRenderer::Stop() {
  sink_->Pause();
}

bool AudioRenderer::NeedReadStream() {
  // FIXME temp solution.
  return audio_buffer_.size() < 3;
}

void AudioRenderer::SetVolume(double volume) {
  DCHECK(volume >= 0);
  DCHECK(volume <= 1);
  std::lock_guard<std::mutex> auto_lock(mutex_);
  volume_ = volume;
}

void AudioRenderer::Flush() {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  audio_buffer_.clear();
  decoder_stream_->Flush();
}

std::ostream &operator<<(std::ostream &os, const AudioRenderer &renderer) {
  os << " audio_buffer_: " << renderer.audio_buffer_.size()
     << " reading_: " << renderer.reading_
     << " volume_: " << renderer.volume_;
  if (renderer.decoder_stream_) {
    os << " decoder_stream( " << *renderer.decoder_stream_ << " )";
  }
  return os;
}

}