//
// Created by yangbin on 2021/4/2.
//

#ifndef MEDIA_PLAYER_PIPELINE_STATUS_H_
#define MEDIA_PLAYER_PIPELINE_STATUS_H_

#include <functional>
#include <string>

#include "base/basictypes.h"

namespace media {

enum class PipelineStatus {
  OK = 0,
  ERROR_URL_NOT_FOUND = 1,
  ERROR_NETWORK = 2,
  ERROR_DECODE = 3,
  ERROR_DECRYPT = 4,
  ERROR_ABORT = 5,
  ERROR_INITIALIZATION_FAILED = 6,
  ERROR_COULD_NOT_RENDER = 8,
  ERROR_READ = 9,
  ERROR_OPERATION_PENDING = 10,
  ERROR_INVALID_STATE = 11,
  // Demuxer related errors.
  DEMUXER_ERROR_COULD_NOT_OPEN = 12,
  DEMUXER_ERROR_COULD_NOT_PARSE = 13,
  DEMUXER_ERROR_NO_SUPPORTED_STREAMS = 14,
  // Decoder related errors.
  DECODER_ERROR_NOT_SUPPORTED = 15,
  STATUS_MAX,  // Must be greater than all other values logged.
};

typedef std::function<void(PipelineStatus)> PipelineStatusCB;

// TODO: this should be moved alongside host interface definitions.
struct PipelineStatistics {
  PipelineStatistics()
      : audio_bytes_decoded(0),
        video_bytes_decoded(0),
        video_frames_decoded(0),
        video_frames_dropped(0) {
  }

  uint32 audio_bytes_decoded;  // Should be uint64?
  uint32 video_bytes_decoded;  // Should be uint64?
  uint32 video_frames_decoded;
  uint32 video_frames_dropped;
};

// Used for updating pipeline statistics.
typedef std::function<void(const PipelineStatistics &)> StatisticsCB;

}

#endif //MEDIA_PLAYER_PIPELINE_STATUS_H_
