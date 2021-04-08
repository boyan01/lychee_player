//
// Created by yangbin on 2021/4/6.
//

#ifndef MEDIA_PLAYER_PIPELINE_PLAYER_PIPELINE_H_
#define MEDIA_PLAYER_PIPELINE_PLAYER_PIPELINE_H_

#include "memory"
#include "string"
#include "functional"

#include "base/basictypes.h"
#include "base/message_loop.h"

#include "decoder/media_tracks.h"
#include "renderer/renderer.h"
#include "source/data_source.h"
#include "demuxer/demuxer.h"

namespace media {

class PlayerPipeline : public DemuxerHost {

 public:

  PlayerPipeline();
  ~PlayerPipeline() override;

  typedef std::function<void(bool)> OpenSourceCB;

  void Open(const std::string &url, OpenSourceCB open_source_cb);

  void SetDuration(chrono::microseconds duration) override;

  void OnDemuxerError(PipelineStatus error) override;

 private:
  std::shared_ptr<Renderer> renderer_;
  std::shared_ptr<DataSource> data_source_;

  std::shared_ptr<Demuxer> demuxer_;

  std::shared_ptr<base::MessageLoop> pipeline_runner_;
  std::shared_ptr<base::MessageLoop> decoder_runner_;

  bool OpenTask(const std::string &url);

  void OnMediaTracksUpdated(std::unique_ptr<MediaTracks> tracks);

  void OnDemuxerInited();

  DISALLOW_COPY_AND_ASSIGN(PlayerPipeline);

};

}

#endif //MEDIA_PLAYER_PIPELINE_PLAYER_PIPELINE_H_
