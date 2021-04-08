//
// Created by yangbin on 2021/4/7.
//

#ifndef MEDIA_PLAYER_PIPELINE_PIPELINE_STATUS_H_
#define MEDIA_PLAYER_PIPELINE_PIPELINE_STATUS_H_

#include "functional"
#include "iosfwd"
#include "string"

namespace media {

// Status states for pipeline.  All codes except PIPELINE_OK indicate errors.
// Logged to UMA, so never reuse a value, always add new/greater ones!
// When adding a new one, also update enums.xml.
enum PipelineStatus {
  PIPELINE_OK = 0,
  // Deprecated: PIPELINE_ERROR_URL_NOT_FOUND = 1,
  PIPELINE_ERROR_NETWORK = 2,
  PIPELINE_ERROR_DECODE = 3,
  // Deprecated: PIPELINE_ERROR_DECRYPT = 4,
  PIPELINE_ERROR_ABORT = 5,
  PIPELINE_ERROR_INITIALIZATION_FAILED = 6,
  PIPELINE_ERROR_COULD_NOT_RENDER = 8,
  PIPELINE_ERROR_READ = 9,
  // Deprecated: PIPELINE_ERROR_OPERATION_PENDING = 10,
  PIPELINE_ERROR_INVALID_STATE = 11,

  // Demuxer related errors.
  DEMUXER_ERROR_COULD_NOT_OPEN = 12,
  DEMUXER_ERROR_COULD_NOT_PARSE = 13,
  DEMUXER_ERROR_NO_SUPPORTED_STREAMS = 14,

  // Decoder related errors.
  DECODER_ERROR_NOT_SUPPORTED = 15,

  // ChunkDemuxer related errors.
  CHUNK_DEMUXER_ERROR_APPEND_FAILED = 16,
  CHUNK_DEMUXER_ERROR_EOS_STATUS_DECODE_ERROR = 17,
  CHUNK_DEMUXER_ERROR_EOS_STATUS_NETWORK_ERROR = 18,

  // Audio rendering errors.
  AUDIO_RENDERER_ERROR = 19,

  // Deprecated: AUDIO_RENDERER_ERROR_SPLICE_FAILED = 20,
  PIPELINE_ERROR_EXTERNAL_RENDERER_FAILED = 21,

  // Android only. Used as a signal to fallback MediaPlayerRenderer, and thus
  // not exactly an 'error' per say.
  DEMUXER_ERROR_DETECTED_HLS = 22,

  // Must be equal to the largest value ever logged.
  PIPELINE_STATUS_MAX = DEMUXER_ERROR_DETECTED_HLS,
};

std::string PipelineStatusToString(PipelineStatus status);

std::ostream &operator<<(std::ostream &out, PipelineStatus status);

using PipelineStatusCallback = std::function<void(PipelineStatus)>;

}

#endif //MEDIA_PLAYER_PIPELINE_PIPELINE_STATUS_H_
