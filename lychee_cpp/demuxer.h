//
// Created by boyan on 22-5-3.
//

#ifndef LYCHEE_PLAYER__DEMUXER_H_
#define LYCHEE_PLAYER__DEMUXER_H_

#include <string>

extern "C" {
#include "libavformat/avformat.h"
}

#include <base/task_runner.h>

#include "demuxer_stream.h"

namespace lychee {

class DemuxerHost {

 public:

  virtual void OnDemuxerBuffering(double progress) = 0;

  virtual void OnDemuxerHasEnoughData() = 0;

  virtual void SetDuration(double duration) = 0;

};

class Demuxer {

 public:

  Demuxer(const media::TaskRunner &task_runner, std::string url, DemuxerHost *host);

  using InitializeCallback = std::function<void(std::shared_ptr<DemuxerStream>)>;
  void Initialize(InitializeCallback callback);

  void NotifyCapacityAvailable();

 private:

  media::TaskRunner task_runner_;

  std::string url_;
  AVFormatContext *format_context_;

  bool abort_request_;

  std::shared_ptr<DemuxerStream> audio_stream_;

  InitializeCallback initialize_callback_;

  DemuxerHost *host_;

  void OnInitializeDone();

  void DemuxTask();

};

} // lychee

#endif //LYCHEE_PLAYER__DEMUXER_H_
