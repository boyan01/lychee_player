#ifndef LYCHEE_MEDIA_DEMUXER_H_
#define LYCHEE_MEDIA_DEMUXER_H_

#include <string>
#include <vector>

#include "media/encoded_packet.h"
#include "media/stream_info.h"

namespace lychee {

enum class DemuxerReadResult { kOk, kEndOfStream, kError, kAborted };

class Demuxer {
 public:
  virtual ~Demuxer() = default;

  virtual const char* name() const = 0;

  // Blocking. Fills media_info() on success.
  virtual bool Open(const std::string& url, std::string* error) = 0;

  virtual const MediaInfo& media_info() const = 0;

  // Only packets of the enabled streams are returned.
  virtual void SetEnabledStreams(const std::vector<int>& stream_indices) = 0;

  // Blocking.
  virtual DemuxerReadResult Read(EncodedPacket* packet) = 0;

  // Seeks to the closest keyframe at or before |time| (media timeline).
  virtual bool Seek(int64_t time) = 0;

  // Thread-safe. Makes pending and future blocking calls return early.
  virtual void Abort() = 0;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DEMUXER_H_
