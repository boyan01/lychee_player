//
// Created by yangbin on 2021/4/5.
//

#include "base/logging.h"
#include "base/lambda.h"

#include "renderer/renderer.h"

#include <utility>

namespace media {

Renderer::Renderer(std::shared_ptr<base::MessageLoop> task_runner,
                   std::unique_ptr<AudioRenderer> audio_renderer,
                   std::unique_ptr<VideoRenderer> video_renderer)
    : audio_renderer_(std::move(audio_renderer)),
      video_renderer_(std::move(video_renderer)),
      task_runner_(std::move(task_runner)) {
  DCHECK(audio_renderer_) << "AudioRender is null";
  DCHECK(video_renderer_) << "VideoRender is null";
  DCHECK(task_runner_);

}

void Renderer::Initialize(Demuxer *media_source, PipelineStatusCallback init_callback) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(init_callback);

  init_callback_ = std::move(init_callback);
  media_source_ = media_source;
  state_ = STATE_INITIALIZING;
  InitializeAudioRender();
}

void Renderer::InitializeAudioRender() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, STATE_INITIALIZING);
  DCHECK(init_callback_);

  auto *audio_stream = media_source_->GetFirstStream(DemuxerStream::AUDIO);
  if (!audio_stream) {
    audio_renderer_.reset();
    task_runner_->PostTask(FROM_HERE, [WEAK_THIS(Renderer)] {
      auto instance = weak_this.lock();
      if (instance) {
        instance->OnAudioRendererInitializeDone(PIPELINE_OK);
      }
    });
    return;
  }

  current_audio_stream_ = audio_stream;

  audio_renderer_->Initialize(audio_stream, [WEAK_THIS(Renderer)](PipelineStatus status) {
    auto render = weak_this.lock();
    if (render) {
      render->OnAudioRendererInitializeDone(status);
    }
  });

}

void Renderer::OnAudioRendererInitializeDone(PipelineStatus status) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  // OnError may be fired first.
  if (state_ != STATE_INITIALIZING) {
    DCHECK(!init_callback_);
    audio_renderer_.reset();
    return;
  }

  if (status != PIPELINE_OK) {
    FinishInitialization(status);
    return;
  }

  DCHECK(init_callback_);
  InitializeVideoRender();
}

void Renderer::FinishInitialization(PipelineStatus status) {
  DCHECK(init_callback_);
  DLOG(INFO) << "Renderer#FinishInitialization:" << "status =" << status;
  std::move(init_callback_)(status);
}

void Renderer::InitializeVideoRender() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, STATE_INITIALIZING);
  DCHECK(init_callback_);

  auto *video_stream = media_source_->GetFirstStream(DemuxerStream::VIDEO);
  if (!video_stream) {
    video_renderer_.reset();
    task_runner_->PostTask(FROM_HERE, [WEAK_THIS(Renderer)]() {
      auto renderer = weak_this.lock();
      if (renderer) {
        renderer->OnVideoRendererInitializeDone(PIPELINE_OK);
      }
    });
    return;
  }

  current_video_stream_ = video_stream;
  video_renderer_->Initialize(video_stream, [WEAK_THIS(Renderer)](PipelineStatus status) {
    auto weak = weak_this.lock();
    if (weak) {
      weak->OnVideoRendererInitializeDone(status);
    }
  });
}

void Renderer::OnVideoRendererInitializeDone(PipelineStatus status) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  // OnError may be fired first.
  if (state_ != STATE_INITIALIZING) {
    DCHECK(!init_callback_);
    audio_renderer_.reset();
    video_renderer_.reset();
    return;
  }

  DCHECK(init_callback_);
  if (status != PIPELINE_OK) {
    FinishInitialization(status);
    return;
  }

  // TODO init clock

  state_ = STATE_FLUSHED;
  DCHECK(audio_renderer_ || video_renderer_);
  FinishInitialization(PIPELINE_OK);
}

}