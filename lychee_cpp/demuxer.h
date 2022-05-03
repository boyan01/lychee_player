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

class Demuxer {

 public:

  Demuxer(const media::TaskRunner &task_runner, std::string url);

  void Initialize();

  void NotifyCapacityAvailable();

 private:

  media::TaskRunner task_runner_;

  std::string url_;
  AVFormatContext *format_context_;

  bool abort_request_;

  std::shared_ptr<DemuxerStream> audio_stream_;

  void OnInitializeDone();

  void DemuxTask();

};

} // lychee

#endif //LYCHEE_PLAYER__DEMUXER_H_
