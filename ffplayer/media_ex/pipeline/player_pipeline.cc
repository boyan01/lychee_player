//
// Created by yangbin on 2021/4/6.
//

#include "pipeline/player_pipeline.h"

#include "base/logging.h"
#include "renderer/audio_renderer.h"
#include "source/file_data_source.h"

namespace media {

using base::MessageLoop;

PlayerPipeline::PlayerPipeline() {
  pipeline_runner_ = std::shared_ptr<MessageLoop>(
      MessageLoop::prepare_looper("media_pipeline"));
}

PlayerPipeline::~PlayerPipeline() { pipeline_runner_->Quit(); }

void PlayerPipeline::Open(const std::string &url,
                          PlayerPipeline::OpenSourceCB open_source_cb) {
  pipeline_runner_->PostTask(FROM_HERE, [&]() {
    auto ret = OpenTask(url);
    open_source_cb(ret);
  });
}

bool PlayerPipeline::OpenTask(const std::string &url) {
  DCHECK(pipeline_runner_->BelongsToCurrentThread());
  auto data_source = std::make_shared<FileDataSource>();
  if (!data_source->Initialize(url)) {
    DLOG(WARNING) << "can not open file " << url;
    return false;
  }
  data_source_ = std::move(data_source);
  decoder_runner_ =
      std::shared_ptr<MessageLoop>(MessageLoop::prepare_looper("decoder"));
  demuxer_ =
      std::make_shared<Demuxer>(decoder_runner_, data_source_.get(),
                                [this](std::unique_ptr<MediaTracks> tracks) {
                                  OnMediaTracksUpdated(std::move(tracks));
                                });
  demuxer_->Initialize(this, [this](int status) {
    DLOG(INFO) << "Status" << status;
    if (status == 0) {
      OnDemuxerInited();
    }
  });
  return true;
}

void PlayerPipeline::OnMediaTracksUpdated(std::unique_ptr<MediaTracks> tracks) {

}

void PlayerPipeline::SetDuration(chrono::microseconds duration) {}

void PlayerPipeline::OnDemuxerError(PipelineStatus error) {}

void PlayerPipeline::OnDemuxerInited() {
  //  auto audio_render = std::make_unique<AudioRenderer>();
  //  auto video_render = std::make_unique<VideoRenderer>();
  //  renderer_ = std::make_shared<Renderer>(pipeline_runner_,
  //  std::move(audio_render), std::move(video_render));
}

}  // namespace media
